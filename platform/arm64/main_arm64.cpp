// ProjectDural — Virtua Fighter 2 (vf2-recomp) ARM64 / OpenGL ES 2.0 / SDL2 Frontend
// Engineered for Anbernic RG34XX (Allwinner H700: 4x Cortex-A53 @ 1.5 GHz,
// Mali-G31 MP2, 1 GB LPDDR4, 720x480 3:2) and compatible PortMaster handhelds.
//
// * Core 0: Main loop, SLJIT ARM64 i960 + TGP + Geometrizer + GLES 2.0 submit
// * Core 1: Auxiliary row_pool worker (2D tile compositing / optional CPU 3D)
// * Core 2: Decoupled SoundWorker (68000 + SCSP + SCSP DSP via set_sound_only)
// * Core 3: SDL2 adaptive-rate audio callback + auxiliary worker
#define SDL_MAIN_HANDLED

#include "audio.h"
#include "controls.h"
#include "gpu_gles.h"
#include "imgui_gles.h"
#include "sound_worker.h"
#include "thread_affinity.h"

#include "gpu/gpu3d.h"
#include "host/backgrounds.h"
#include "host/romset.h"
#include "machine/model2a.h"
#include "machine/model2_video.h"

// For --bench-synthetic ROM-free verification
#include "../tools/synthetic_rom.h"

#include <SDL.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kMainCore = 0;
constexpr int kSoundCore = 2;

uint64_t ticks_us() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

void ensure_dir(const std::string &path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

std::string executable_dir(const char *argv0) {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, size_t(n)) : (argv0 ? std::string(argv0) : std::string("."));
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(data, 1, size, f) == size && std::fgetc(f) == EOF;
    std::fclose(f);
    return ok;
}

bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string load_text(const std::string &path) {
    std::ifstream f(path);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void save_text(const std::string &path, const std::string &txt) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = txt.empty() || std::fwrite(txt.data(), 1, txt.size(), f) == txt.size();
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); return; }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

constexpr const char *kBindKeys[arm64::ActionCount] = {
    "bind_guard", "bind_punch", "bind_kick",
    "bind_guard_punch", "bind_punch_kick", "bind_guard_kick", "bind_all_three",
    "bind_start", "bind_coin"
};

struct Settings {
    int aspect = 0;                 // 0 = 3:2 Widescreen, 1 = 4:3 Original, 2 = Stretch
    bool gpu_3d = true;             // true = Mali-G31 GLES 2.0, false = CPU rasterizer
    bool backdrop_filter = true;    // smooth stage sky/backdrop
    bool hide_hud = false;
    bool vsync = true;
    bool sync_60hz = false;         // false = 57.524 Hz arcade pacing, true = 60 Hz display sync
    bool show_fps = false;
    int volume = 90;
    int music_volume = 100;
    int effects_volume = 100;
    bool mute = false;
    bool dsp_reverb = true;

    // Arcade & Cheats
    bool free_play = true;
    bool fast_boot = true;
    bool debug_mode = false;
    int secret_char = 0;            // 0=Off, 1=Dural(9), 2=MUE(11), 3=KOJAC(12)
    int random_mode = 0;            // 0=Off, 1=P1 Random, 2=CPU Opponents, 3=Both
    int cheat_inf_energy = 0;       // 0=Off, 1=P1, 2=P2, 3=Both
    bool cheat_one_hit = false;
    bool cheat_no_time = false;
    int cheat_ring = 0;             // 0=Normal, 1=Tiny, 2=Huge
    int cheat_bird = 0;             // 0=Arcade, 1=1P Code, 2=Auto

    arm64::Bindings bindings;

    void defaults() { *this = Settings{}; }

    void sanitize() {
        aspect = std::clamp(aspect, 0, 2);
        volume = std::clamp(volume, 0, 100);
        music_volume = std::clamp(music_volume, 0, 100);
        effects_volume = std::clamp(effects_volume, 0, 100);
        secret_char = std::clamp(secret_char, 0, 3);
        random_mode = std::clamp(random_mode, 0, 3);
        cheat_inf_energy = std::clamp(cheat_inf_energy, 0, 3);
        cheat_ring = std::clamp(cheat_ring, 0, 2);
        cheat_bird = std::clamp(cheat_bird, 0, 2);
        bindings.sanitize();
    }

    void load(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char line[128], key[48], sval[64];
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%47[^=]=%63s", key, sval) != 2) continue;
            const int v = std::atoi(sval);
            if (!std::strcmp(key, "aspect")) aspect = v;
            else if (!std::strcmp(key, "gpu_3d")) gpu_3d = v != 0;
            else if (!std::strcmp(key, "backdrop_filter")) backdrop_filter = v != 0;
            else if (!std::strcmp(key, "hide_hud")) hide_hud = v != 0;
            else if (!std::strcmp(key, "vsync")) vsync = v != 0;
            else if (!std::strcmp(key, "sync_60hz")) sync_60hz = v != 0;
            else if (!std::strcmp(key, "show_fps")) show_fps = v != 0;
            else if (!std::strcmp(key, "volume")) volume = v;
            else if (!std::strcmp(key, "music_volume")) music_volume = v;
            else if (!std::strcmp(key, "effects_volume")) effects_volume = v;
            else if (!std::strcmp(key, "mute")) mute = v != 0;
            else if (!std::strcmp(key, "dsp_reverb")) dsp_reverb = v != 0;
            else if (!std::strcmp(key, "free_play")) free_play = v != 0;
            else if (!std::strcmp(key, "fast_boot")) fast_boot = v != 0;
            else if (!std::strcmp(key, "debug_mode")) debug_mode = v != 0;
            else if (!std::strcmp(key, "secret_char")) secret_char = v;
            else if (!std::strcmp(key, "random_mode")) random_mode = v;
            else if (!std::strcmp(key, "cheat_inf_energy")) cheat_inf_energy = v;
            else if (!std::strcmp(key, "cheat_one_hit")) cheat_one_hit = v != 0;
            else if (!std::strcmp(key, "cheat_no_time")) cheat_no_time = v != 0;
            else if (!std::strcmp(key, "cheat_ring")) cheat_ring = v;
            else if (!std::strcmp(key, "cheat_bird")) cheat_bird = v;
            else {
                for (int a = 0; a < arm64::ActionCount; ++a)
                    if (!std::strcmp(key, kBindKeys[a])) bindings.button[a] = uint32_t(std::strtoul(sval, nullptr, 10));
            }
        }
        std::fclose(f);
        sanitize();
    }

    bool save(const std::string &path) const {
        const std::string tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "aspect=%d\ngpu_3d=%d\nbackdrop_filter=%d\nhide_hud=%d\nvsync=%d\nsync_60hz=%d\nshow_fps=%d\n",
                     aspect, int(gpu_3d), int(backdrop_filter), int(hide_hud), int(vsync), int(sync_60hz), int(show_fps));
        std::fprintf(f, "volume=%d\nmusic_volume=%d\neffects_volume=%d\nmute=%d\ndsp_reverb=%d\n",
                     volume, music_volume, effects_volume, int(mute), int(dsp_reverb));
        std::fprintf(f, "free_play=%d\nfast_boot=%d\ndebug_mode=%d\nsecret_char=%d\nrandom_mode=%d\n",
                     int(free_play), int(fast_boot), int(debug_mode), secret_char, random_mode);
        std::fprintf(f, "cheat_inf_energy=%d\ncheat_one_hit=%d\ncheat_no_time=%d\ncheat_ring=%d\ncheat_bird=%d\n",
                     cheat_inf_energy, int(cheat_one_hit), int(cheat_no_time), cheat_ring, cheat_bird);
        for (int a = 0; a < arm64::ActionCount; ++a)
            std::fprintf(f, "%s=%u\n", kBindKeys[a], unsigned(bindings.button[a]));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove(tmp.c_str()); return false; }
        std::remove(path.c_str());
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
};

struct CliOptions {
    std::string data_dir;
    std::string explicit_rom;
    bool fullscreen = true;
    bool show_fps = false;
    bool interp = false;
    bool arcade = false;
    int bench_frames = 0;
    int bench_synthetic = 0;
};

void print_usage(const char *prog) {
    std::printf(
        "ProjectDural — Virtua Fighter 2 (vf2-recomp) ARM64 / OpenGL ES 2.0 Runtime\n"
        "Usage: %s [options]\n"
        "  --data-dir PATH        Data folder containing roms/, saves/, assets/\n"
        "  --rom FILE.zip         Override MAME vf2.zip path\n"
        "  --fullscreen           Launch fullscreen (default)\n"
        "  --windowed             Launch in a 720x480 window\n"
        "  --fps                  Enable FPS readout\n"
        "  --interp               Force i960 interpreter instead of SLJIT native code\n"
        "  --arcade               Authentic cabinet boot & coin mode\n"
        "  --bench-frames N       Headless benchmark using vf2.zip\n"
        "  --bench-synthetic N    ROM-free headless verification (SLJIT + SoundWorker + 3D export)\n"
        "  --help                 Show this help message\n",
        prog);
}

int run_synthetic_bench(int frames) {
    romset::result r = synthetic::roms();
    model2a m(std::move(r), true);
    m.set_sound(false);
    m.set_byte_link(true);
    m.set_draw_3d(false);

    std::vector<uint8_t> uart;
    m.on_uart_tx = [&](uint8_t b) { uart.push_back(b); };

    arm64::SoundWorker snd;
    snd.set_core(kSoundCore);
    snd.open(synthetic::roms(), nullptr);
    m.reset();

    const auto &st = m.native_stats();
    std::printf("[synthetic] SLJIT native: %zu functions, %zu/%zu insns native, %zu code bytes (%.2f ms)\n",
                st.functions, st.native, st.instructions, st.code_bytes, st.ms);

    const uint64_t t0 = ticks_us();
    for (int f = 0; f < frames; ++f) {
        uart.clear();
        m.run_frame();
        snd.wait();
        snd.submit(std::move(uart));
    }
    snd.wait();
    const double emu_ms = double(ticks_us() - t0) / 1000.0;
    std::printf("[synthetic] Ran %d frames in %.2f ms (%.1f FPS), Core 2 sound samples=%llu\n",
                frames, emu_ms, double(frames) * 1000.0 / std::max(emu_ms, 0.001),
                (unsigned long long)snd.samples_total());
    snd.close();

    // Verify 3D polygon pipeline (1500 polygons)
    std::vector<u32> buf(0x10000, 0), tex0(0x80000, 0), tex1(0x80000, 0);
    std::vector<u8> luma(0x8000, 0x20), texrom(0x1000, 0), polyrom(0x1000, 0);
    std::vector<u16> pal(0x2000, 0x7fff), xlat(0x6000, 0xff);
    u8 gamma[256];
    for (int i = 0; i < 256; ++i) gamma[i] = uint8_t(i);
    model2_video v(buf.data(), tex0.data(), tex1.data(), luma.data(), pal.data(), xlat.data(), gamma,
                   texrom.data(), texrom.size(), polyrom.data(), polyrom.size());
    v.bench_make(1500);
    std::vector<gpu3d_poly> exported;
    v.bench_export(exported);
    std::printf("[synthetic] 3D export check: %zu gpu3d_poly records ready for GLES 2.0\n", exported.size());
    return (st.native > 0 && exported.size() >= 1500) ? 0 : 1;
}

int run_rom_bench(const std::string &rom_path, int frames, bool interp) {
    romset::result roms = romset::load(rom_path);
    if (!roms.ok) {
        std::fprintf(stderr, "[bench] ROM load error: %s\n", roms.error.c_str());
        return 1;
    }
    arm64::SoundWorker snd;
    snd.set_core(kSoundCore);
    snd.open(roms, nullptr);

    model2a m(std::move(roms), !interp);
    m.set_sound(false);
    m.set_byte_link(true);
    m.set_skip_z80(true);
    m.set_external_gpu(true);
    std::vector<uint8_t> uart;
    m.on_uart_tx = [&](uint8_t b) { uart.push_back(b); };
    m.reset();

    const auto &st = m.native_stats();
    std::printf("[bench] SLJIT: %zu funcs, %zu/%zu native (%.1f ms)\n",
                st.functions, st.native, st.instructions, st.ms);

    const uint64_t t0 = ticks_us();
    uint64_t emu_us = 0, snd_us = 0, ren_us = 0;
    size_t total_polys = 0;
    std::vector<gpu3d_poly> polys;
    model2a::picture_parts parts;
    model2a::gpu3d_hook hook = [&](model2_video &v, int pic) {
        v.export_picture(pic, polys);
        total_polys += polys.size();
        return true;
    };
    for (int f = 0; f < frames; ++f) {
        const uint64_t a = ticks_us();
        uart.clear();
        m.run_frame();
        const uint64_t b = ticks_us();
        snd.wait();
        snd.submit(std::move(uart));
        const uint64_t c = ticks_us();
        m.render_parts(-1.0f, nullptr, 0, false, parts, &hook, false, false);
        const uint64_t d = ticks_us();
        emu_us += (b - a);
        snd_us += (c - b);
        ren_us += (d - c);
    }
    snd.wait();
    snd.close();
    const double ms = double(ticks_us() - t0) / 1000.0;
    std::printf("[bench] %d frames in %.2f ms (%.2f FPS), avg polys=%.1f | emu=%.2fms snd_wait=%.2fms export=%.2fms\n",
                frames, ms, double(frames) * 1000.0 / std::max(ms, 0.001), double(total_polys) / frames,
                double(emu_us) / (1000.0 * frames), double(snd_us) / (1000.0 * frames), double(ren_us) / (1000.0 * frames));
    return 0;
}

std::string find_rom(const std::string &data_dir, const std::string &explicit_rom) {
    if (!explicit_rom.empty()) return explicit_rom;
    const std::string candidates[] = {
        data_dir + "/roms/vf2.zip",
        data_dir + "/vf2.zip",
        "roms/vf2.zip",
    };
    for (const auto &c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) return c;
    }
    return "";
}

int secret_code_from_idx(int idx) {
    static constexpr int codes[] = {0, 9, 11, 12};
    return (idx >= 0 && idx < 4) ? codes[idx] : 0;
}

void apply_board_settings(model2a &m, const Settings &s, arm64::SoundWorker &snd) {
    const float asp = (s.aspect == 0) ? 1.5f : (4.0f / 3.0f);
    m.set_aspect(asp);
    m.set_external_gpu(s.gpu_3d);
    m.set_backdrop_filter(s.backdrop_filter);
    m.set_hud_hidden(s.hide_hud);
    m.set_debug_mode(s.debug_mode);
    snd.set_volumes(float(s.music_volume) / 100.0f, float(s.effects_volume) / 100.0f);
    snd.set_dsp_enabled(s.dsp_reverb);
}

void save_nvram(const model2a &m, const std::string &saves_dir) {
    ensure_dir(saves_dir);
    save_bytes(saves_dir + "/backup.bin", m.backup_ram().data(), m.backup_ram().size());
    save_bytes(saves_dir + "/eeprom.bin", reinterpret_cast<const uint8_t *>(m.eeprom_cells()), 64 * sizeof(uint16_t));
    save_text(saves_dir + "/ranking_names.txt", m.ranking_names());
}

void load_nvram(model2a &m, const std::string &saves_dir) {
    load_bytes(saves_dir + "/backup.bin", m.backup_ram().data(), m.backup_ram().size());
    load_bytes(saves_dir + "/eeprom.bin", reinterpret_cast<uint8_t *>(m.eeprom_cells()), 64 * sizeof(uint16_t));
    const std::string rnames = load_text(saves_dir + "/ranking_names.txt");
    if (!rnames.empty()) m.set_ranking_names(rnames);
}

} // namespace

int main(int argc, char **argv) {
    CliOptions cli;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a == "--data-dir" && i + 1 < argc) cli.data_dir = argv[++i];
        else if (a == "--rom" && i + 1 < argc) cli.explicit_rom = argv[++i];
        else if (a == "--fullscreen") cli.fullscreen = true;
        else if (a == "--windowed") cli.fullscreen = false;
        else if (a == "--fps") cli.show_fps = true;
        else if (a == "--interp") cli.interp = true;
        else if (a == "--arcade") cli.arcade = true;
        else if (a == "--bench-frames" && i + 1 < argc) cli.bench_frames = std::max(1, std::atoi(argv[++i]));
        else if (a == "--bench-synthetic" && i + 1 < argc) cli.bench_synthetic = std::max(1, std::atoi(argv[++i]));
    }

    arm64::pin_current_thread(kMainCore);
    arm64::set_current_thread_name("dural-main");
    setenv("VF2_THREADS", "2", 0);

    if (cli.bench_synthetic > 0) return run_synthetic_bench(cli.bench_synthetic);

    std::string data_dir = cli.data_dir.empty() ? executable_dir(argv[0]) : cli.data_dir;
    if (cli.bench_frames > 0) {
        const std::string rp = find_rom(data_dir, cli.explicit_rom);
        if (rp.empty()) { std::fprintf(stderr, "vf2.zip not found in %s/roms/\n", data_dir.c_str()); return 1; }
        return run_rom_bench(rp, cli.bench_frames, cli.interp);
    }

    ensure_dir(data_dir + "/roms");
    ensure_dir(data_dir + "/saves");
    const std::string settings_path = data_dir + "/saves/settings.ini";
    Settings cfg;
    cfg.load(settings_path);
    if (cli.show_fps) cfg.show_fps = true;
    if (cli.arcade) { cfg.free_play = false; cfg.fast_boot = false; }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    arm64::GlConfig gl_cfg;
    gl_cfg.fullscreen = cli.fullscreen;
    gl_cfg.vsync = cfg.vsync;
    if (!arm64::gl_init(gl_cfg)) {
        std::fprintf(stderr, "OpenGL init failed: %s\n", arm64::gl_error());
        SDL_Quit();
        return 1;
    }

    arm64::GpuGlesRenderer gpu;
    arm64::ImGuiGles imgui;
    if (!gpu.ok() || !imgui.init()) {
        std::fprintf(stderr, "Renderer / ImGui init failed\n");
        imgui.shutdown();
        gpu.shutdown();
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::PadManager pads;
    pads.init(data_dir);

    const std::string rom_path = find_rom(data_dir, cli.explicit_rom);
    romset::result roms;
    std::string rom_error;
    if (rom_path.empty()) {
        rom_error = "Missing ROM archive: place MAME vf2.zip (Version 2.1) in:\n" + data_dir + "/roms/vf2.zip";
    } else {
        roms = romset::load(rom_path);
        if (!roms.ok) rom_error = "Invalid vf2.zip:\n" + roms.error;
    }

    // Missing ROM graceful UI loop
    if (!rom_error.empty()) {
        bool quit = false;
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) quit = true;
                else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN)) quit = true;
                else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) arm64::gl_update_drawable_size();
                else pads.handle_event(e);
            }
            const uint32_t b0 = pads.poll_player(0, SDL_GetKeyboardState(nullptr));
            if (b0 & (arm64::BtnA | arm64::BtnB | arm64::Start | arm64::Guide)) quit = true;
            arm64::gl_begin_frame();
            imgui.frame(1.0f / 60.0f);
            ImGui::SetNextWindowPos({float(arm64::gl_display_width()) * 0.5f, float(arm64::gl_display_height()) * 0.5f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({std::min(640.0f, float(arm64::gl_display_width()) - 40.0f), 0.0f});
            ImGui::Begin("ProjectDural — ROM Required", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
            ImGui::TextWrapped("%s", rom_error.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Zero ROMs are bundled. Supply your own MAME vf2.zip.");
            ImGui::Spacing();
            if (ImGui::Button("Exit (Press Start / A / Esc)", {-1.0f, 0.0f})) quit = true;
            ImGui::End();
            imgui.render();
            arm64::gl_end_frame();
            SDL_Delay(16);
        }
        imgui.shutdown();
        gpu.shutdown();
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::Audio audio;
    const bool audio_ok = audio.open(model2a::SOUND_RATE);
    if (audio_ok) {
        audio.volume(float(cfg.volume) / 100.0f);
        audio.mute(cfg.mute);
    }

    arm64::SoundWorker sound_worker;
    sound_worker.set_core(kSoundCore);
    sound_worker.open(roms, audio_ok ? &audio : nullptr);

    std::unique_ptr<model2a> game = std::make_unique<model2a>(std::move(roms), !cli.interp);
    game->set_sound(false);
    game->set_byte_link(true);
    game->set_skip_z80(true);

    std::vector<uint8_t> uart_bytes;
    game->on_uart_tx = [&](uint8_t b) { uart_bytes.push_back(b); };

    // Load painted widescreen sides for character select
    load_backgrounds(*game, data_dir + "/assets/backgrounds");
    load_nvram(*game, data_dir + "/saves");

    auto restart_game = [&](bool skip_boot) {
        sound_worker.wait();
        sound_worker.reset();
        game->forget_boot();
        if (cfg.free_play) {
            game->force_free_play();
            game->hide_free_play_text();
        } else {
            game->show_free_play_text();
        }
        if (cfg.fast_boot) game->shorten_warning();
        else game->restore_warning();

        game->set_secret_character(secret_code_from_idx(cfg.secret_char));
        game->set_random_player(0, (cfg.random_mode == 1 || cfg.random_mode == 3) ? "*" : "");
        game->set_random_opponents(cfg.random_mode == 2 || cfg.random_mode == 3);
        game->apply_random_fighters();

        game->reset();
        apply_board_settings(*game, cfg, sound_worker);
        gpu.reset_caches();

        if (skip_boot && cfg.fast_boot) {
            for (int f = 0; f < 160 && !game->past_boot(); ++f) {
                uart_bytes.clear();
                game->run_frame();
                sound_worker.wait();
                sound_worker.submit(std::move(uart_bytes));
            }
            sound_worker.wait();
        }
    };

    restart_game(true);

    enum class MenuPage { Main, Display, Audio, Arcade, Cheats, Controls };
    bool running = true;
    bool menu_open = false;
    MenuPage menu_page = MenuPage::Main;
    int menu_idx = 0;
    bool menu_latch = false;
    bool test_hold_armed = false;
    int test_hold_frames = 0;
    uint32_t prev_p0_menu = 0;
    uint32_t prev_rank_ver = game->ranking_names_version();

    std::vector<gpu3d_poly> frame_polys;
    gpu3d_rams frame_rams{};
    std::vector<u32> cpu_fb;
    model2a::gpu3d_hook gpu_hook = [&](model2_video &vid, int pic) {
        vid.export_picture(pic, frame_polys);
        vid.gpu_rams(frame_rams);
        return true;
    };

    uint64_t next_frame_us = ticks_us();
    uint64_t fps_t0 = next_frame_us;
    uint64_t log_t0 = next_frame_us;
    int fps_frames = 0;
    int log_frames = 0;
    double current_fps = 57.524;
    double last_emu_ms = 0.0, last_ren_ms = 0.0, last_swap_ms = 0.0;
    double sum_emu_ms = 0.0, sum_snd_ms = 0.0, sum_ren_ms = 0.0, sum_swap_ms = 0.0;
    double sum_polys = 0.0, sum_batches = 0.0;
    uint32_t prev_tex_clr = gpu.last_tex_clears(), prev_pal_clr = gpu.last_pal_clears();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                arm64::gl_update_drawable_size();
            else pads.handle_event(e);
        }

        const uint8_t *keys = SDL_GetKeyboardState(nullptr);
        const uint32_t p0_btns = pads.poll_player(0, keys);
        const uint32_t p1_btns = pads.poll_player(1, keys);

        const bool chord_now = arm64::menu_chord(p0_btns);
        if (chord_now && !menu_latch) {
            menu_open = !menu_open;
            menu_page = MenuPage::Main;
            menu_idx = 0;
            if (menu_open && audio_ok) audio.pause();
            if (!menu_open) cfg.save(settings_path);
            next_frame_us = ticks_us();
        }
        menu_latch = chord_now;

        if (!menu_open) {
            uint8_t in0 = 0xff, in1 = 0xff, in2 = 0xff;
            const bool sys_combo = (p0_btns & arm64::Select) != 0;
            if (sys_combo && (p0_btns & arm64::BtnY)) in0 &= ~arm64::IN0_TEST;
            if (sys_combo && (p0_btns & arm64::BtnX)) in0 &= ~arm64::IN0_SERVICE;
            if (test_hold_frames > 0) {
                in0 &= ~arm64::IN0_TEST;
                --test_hold_frames;
            }
            arm64::apply_player_buttons(p0_btns, cfg.bindings, false, in0, in1, sys_combo);
            arm64::apply_player_buttons(p1_btns, cfg.bindings, true, in0, in2, false);
            game->m_in0 = in0;
            game->m_in1 = in1;
            game->m_in2 = in2;
            game->m_sw = 0xff;

            game->fight_cheats(cfg.cheat_inf_energy, cfg.cheat_one_hit, cfg.cheat_no_time, cfg.cheat_ring, cfg.cheat_bird);

            const uint64_t t_emu0 = ticks_us();
            uart_bytes.clear();
            game->run_frame();
            last_emu_ms = double(ticks_us() - t_emu0) / 1000.0;

            sound_worker.wait();
            sound_worker.submit(std::move(uart_bytes));

            if (game->ranking_names_version() != prev_rank_ver) {
                prev_rank_ver = game->ranking_names_version();
                save_text(data_dir + "/saves/ranking_names.txt", game->ranking_names());
            }

            // Render frame
            const uint64_t t_ren0 = ticks_us();
            arm64::gl_begin_frame();
            const arm64::AspectMode aspect_mode = static_cast<arm64::AspectMode>(cfg.aspect);
            model2a::picture_parts parts;
            const bool got_parts = game->render_parts(-1.0f, nullptr, 0, false, parts,
                                                      cfg.gpu_3d ? &gpu_hook : nullptr, false, false);
            if (got_parts) {
                gpu.draw_frame(parts, game->width(), aspect_mode,
                               frame_polys.data(), frame_polys.size(), &frame_rams);
            } else {
                const int w = game->width();
                cpu_fb.resize(size_t(w) * 384u);
                game->render_into(cpu_fb.data(), size_t(w), true);
                gpu.draw_cpu_framebuffer(cpu_fb.data(), w, 384, aspect_mode);
            }
            last_ren_ms = double(ticks_us() - t_ren0) / 1000.0;

            if (cfg.show_fps) {
                imgui.frame(1.0f / 60.0f);
                ImGui::SetNextWindowPos({8.0f, 8.0f}, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.65f);
                ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
                ImGui::Text("FPS: %.1f | Emu:%.1f GPU:%.1f Swap:%.1f Snd:%.1f | P:%zu(%zu)",
                            current_fps, last_emu_ms, last_ren_ms, last_swap_ms,
                            sound_worker.last_work_ms(), gpu.last_poly_count(), gpu.last_batch_count());
                ImGui::End();
                imgui.render();
            }
            const uint64_t t_swap0 = ticks_us();
            arm64::gl_end_frame();
            const uint64_t now = ticks_us();
            last_swap_ms = double(now - t_swap0) / 1000.0;

            ++fps_frames;
            ++log_frames;
            sum_emu_ms += last_emu_ms;
            sum_snd_ms += sound_worker.last_work_ms();
            sum_ren_ms += last_ren_ms;
            sum_swap_ms += last_swap_ms;
            sum_polys += double(gpu.last_poly_count());
            sum_batches += double(gpu.last_batch_count());

            if (now - fps_t0 >= 500000ull) {
                current_fps = double(fps_frames) * 1000000.0 / double(now - fps_t0);
                fps_frames = 0;
                fps_t0 = now;
            }
            if (now - log_t0 >= 5000000ull && log_frames > 0) {
                const double lf = double(log_frames) * 1000000.0 / double(now - log_t0);
                const uint32_t tc = gpu.last_tex_clears();
                const uint32_t pc = gpu.last_pal_clears();
                std::fprintf(stderr,
                             "[perf] fps=%.1f emu=%.2fms snd=%.2fms gpu=%.2fms swap=%.2fms polys=%.0f batches=%.0f tex=%.1fMB tex_clr=%u pal_clr=%u\n",
                             lf, sum_emu_ms / log_frames, sum_snd_ms / log_frames, sum_ren_ms / log_frames,
                             sum_swap_ms / log_frames, sum_polys / log_frames, sum_batches / log_frames,
                             double(gpu.cached_tex_bytes()) / (1024.0 * 1024.0),
                             tc - prev_tex_clr, pc - prev_pal_clr);
                prev_tex_clr = tc;
                prev_pal_clr = pc;
                log_frames = 0;
                log_t0 = now;
                sum_emu_ms = sum_snd_ms = sum_ren_ms = sum_swap_ms = sum_polys = sum_batches = 0.0;
            }
            if (!cfg.vsync) {
                const uint64_t period_us = cfg.sync_60hz ? 16667ull : 17384ull;
                next_frame_us += period_us;
                if (now < next_frame_us) {
                    const uint64_t wait_ms = (next_frame_us - now) / 1000ull;
                    if (wait_ms > 1) SDL_Delay(Uint32(wait_ms - 1));
                } else if (now - next_frame_us > period_us * 4) {
                    next_frame_us = now;
                }
            } else {
                next_frame_us = now;
            }
        } else {
            // Menu navigation with D-pad + A/B
            const uint32_t edge = p0_btns & ~prev_p0_menu;
            prev_p0_menu = p0_btns;
            auto go_back = [&]() {
                if (menu_page != MenuPage::Main) { menu_page = MenuPage::Main; menu_idx = 0; }
                else { menu_open = false; cfg.save(settings_path); next_frame_us = ticks_us(); }
            };
            if (edge & arm64::BtnB) go_back();

            const int item_counts[] = {8, 7, 5, 7, 7, arm64::ActionCount + 2};
            const int n_items = item_counts[int(menu_page)];
            if (edge & arm64::Up) menu_idx = (menu_idx + n_items - 1) % n_items;
            if (edge & arm64::Down) menu_idx = (menu_idx + 1) % n_items;
            const int lr = (edge & arm64::Right) ? 1 : ((edge & arm64::Left) ? -1 : 0);
            const bool act = (edge & (arm64::BtnA | arm64::Start)) != 0;

            arm64::gl_begin_frame();
            imgui.frame(1.0f / 60.0f);
            ImGui::SetNextWindowPos({float(arm64::gl_display_width()) * 0.5f, float(arm64::gl_display_height()) * 0.5f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({std::min(560.0f, float(arm64::gl_display_width()) - 24.0f), 0.0f});
            const char *titles[] = {
                "ProjectDural — Virtua Fighter 2", "Display Settings", "Audio Settings",
                "Arcade Settings", "Cheats & Secrets", "Controls (P1)"
            };
            ImGui::Begin(titles[int(menu_page)], nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
            ImGui::TextDisabled("%s | SLJIT: %zu insns", arm64::gl_renderer_string(), game->native_stats().native);
            ImGui::Separator();

            auto row = [&](int idx, const char *label, const char *val) -> bool {
                const bool sel = (menu_idx == idx);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.68f, 0.96f, 0.90f));
                const bool clicked = ImGui::Selectable(label, sel);
                if (sel) ImGui::PopStyleColor();
                if (val && *val) {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160.0f);
                    ImGui::Text("%s", val);
                }
                return clicked || (sel && (act || lr != 0));
            };

            if (menu_page == MenuPage::Main) {
                if (row(0, "Resume Game", nullptr) && (act || ImGui::IsItemClicked())) {
                    menu_open = false; cfg.save(settings_path); next_frame_us = ticks_us();
                }
                if (row(1, "Display", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Display; menu_idx = 0; }
                if (row(2, "Audio", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Audio; menu_idx = 0; }
                if (row(3, "Arcade Settings", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Arcade; menu_idx = 0; }
                if (row(4, "Cheats & Secrets", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Cheats; menu_idx = 0; }
                if (row(5, "Controls", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Controls; menu_idx = 0; }
                if (row(6, "Reset Machine", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
                if (row(7, "Quit ProjectDural", nullptr) && (act || ImGui::IsItemClicked())) running = false;
            } else if (menu_page == MenuPage::Display) {
                const char *asp_names[] = {"3:2 Widescreen", "4:3 Original", "Stretch Fill"};
                if (row(0, "Aspect Ratio", asp_names[cfg.aspect])) {
                    cfg.aspect = (cfg.aspect + (lr < 0 ? 2 : 1)) % 3;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(1, "3D Renderer", cfg.gpu_3d ? "GLES 2.0 (Mali)" : "CPU Software")) { cfg.gpu_3d = !cfg.gpu_3d; game->set_external_gpu(cfg.gpu_3d); }
                if (row(2, "Backdrop Filter", cfg.backdrop_filter ? "Smooth" : "Sharp")) {
                    cfg.backdrop_filter = !cfg.backdrop_filter;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(3, "Hide HUD (Fight)", cfg.hide_hud ? "ON" : "OFF")) {
                    cfg.hide_hud = !cfg.hide_hud;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(4, "VSync", cfg.vsync ? "ON" : "OFF")) {
                    cfg.vsync = !cfg.vsync;
                    arm64::gl_set_vsync(cfg.vsync);
                }
                if (row(5, "Frame Pacing", cfg.sync_60hz ? "60.0 Hz" : "57.52 Hz Arcade")) cfg.sync_60hz = !cfg.sync_60hz;
                if (row(6, "Show FPS / Stats", cfg.show_fps ? "ON" : "OFF")) cfg.show_fps = !cfg.show_fps;
            } else if (menu_page == MenuPage::Audio) {
                char vbuf[32];
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.volume);
                if (row(0, "Master Volume", vbuf)) {
                    cfg.volume = std::clamp(cfg.volume + (lr ? lr * 5 : 10), 0, 100);
                    if (audio_ok) audio.volume(float(cfg.volume) / 100.0f);
                }
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.music_volume);
                if (row(1, "Music Volume", vbuf)) {
                    cfg.music_volume = std::clamp(cfg.music_volume + (lr ? lr * 5 : 10), 0, 100);
                    apply_board_settings(*game, cfg, sound_worker);
                }
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.effects_volume);
                if (row(2, "SFX / Voice Volume", vbuf)) {
                    cfg.effects_volume = std::clamp(cfg.effects_volume + (lr ? lr * 5 : 10), 0, 100);
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(3, "Mute Audio", cfg.mute ? "ON" : "OFF")) {
                    cfg.mute = !cfg.mute;
                    if (audio_ok) audio.mute(cfg.mute);
                }
                if (row(4, "SCSP Stereo Reverb DSP", cfg.dsp_reverb ? "ON" : "OFF (Fast)")) {
                    cfg.dsp_reverb = !cfg.dsp_reverb;
                    apply_board_settings(*game, cfg, sound_worker);
                }
            } else if (menu_page == MenuPage::Arcade) {
                if (row(0, "Coin Mode", cfg.free_play ? "Free Play" : "Coin / Cabinet")) {
                    cfg.free_play = !cfg.free_play;
                    restart_game(true);
                }
                const int diff = std::clamp(game->assignment(model2a::game_assignment::difficulty), 0, 3);
                const char *dnames[] = {"Easy", "Normal", "Hard", "Hardest"};
                if (row(1, "Difficulty", dnames[diff])) {
                    game->set_assignment(model2a::game_assignment::difficulty, (diff + (lr < 0 ? 3 : 1)) % 4);
                }
                const int r1p = std::clamp(game->assignment(model2a::game_assignment::match_1p), 1, 5);
                char rbuf[16]; std::snprintf(rbuf, sizeof rbuf, "%d", r1p);
                if (row(2, "Rounds to Win (1P)", rbuf)) {
                    game->set_assignment(model2a::game_assignment::match_1p, (r1p % 5) + 1);
                }
                const int ver = std::clamp(game->assignment(model2a::game_assignment::version), 0, 1);
                if (row(3, "Game Version", ver ? "Version 2.1" : "Original")) {
                    game->set_assignment(model2a::game_assignment::version, ver ^ 1);
                }
                if (row(4, "Debug Mode", cfg.debug_mode ? "ON" : "OFF")) {
                    cfg.debug_mode = !cfg.debug_mode;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(5, "Hold Test Button (Next Resume)", test_hold_armed ? "ARMED" : "OFF")) {
                    test_hold_armed = !test_hold_armed;
                    if (test_hold_armed) test_hold_frames = 30;
                    else test_hold_frames = 0;
                }
                if (row(6, "Apply & Restart Machine", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
            } else if (menu_page == MenuPage::Cheats) {
                const char *snames[] = {"Off (Akira)", "Dural", "MUE", "KOJAC"};
                if (row(0, "Secret Character", snames[cfg.secret_char])) {
                    cfg.secret_char = (cfg.secret_char + (lr < 0 ? 3 : 1)) % 4;
                }
                const char *rmodes[] = {"Off", "P1 Random", "CPU Opponents", "Both"};
                if (row(1, "Random Fighters", rmodes[cfg.random_mode])) {
                    cfg.random_mode = (cfg.random_mode + (lr < 0 ? 3 : 1)) % 4;
                }
                const char *infs[] = {"Off", "Player 1", "Player 2", "Both"};
                if (row(2, "Infinite Energy", infs[cfg.cheat_inf_energy])) {
                    cfg.cheat_inf_energy = (cfg.cheat_inf_energy + (lr < 0 ? 3 : 1)) % 4;
                }
                if (row(3, "One-Hit KO", cfg.cheat_one_hit ? "ON" : "OFF")) cfg.cheat_one_hit = !cfg.cheat_one_hit;
                if (row(4, "No Time Limit", cfg.cheat_no_time ? "ON" : "OFF")) cfg.cheat_no_time = !cfg.cheat_no_time;
                const char *rings[] = {"Normal", "Tiny (650)", "Huge (No Ringout)"};
                if (row(5, "Ring Width", rings[cfg.cheat_ring])) {
                    cfg.cheat_ring = (cfg.cheat_ring + (lr < 0 ? 2 : 1)) % 3;
                }
                if (row(6, "Apply & Restart (Secrets/Random)", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
            } else if (menu_page == MenuPage::Controls) {
                for (int a = 0; a < arm64::ActionCount; ++a) {
                    if (row(a, arm64::action_name(a), arm64::button_name(cfg.bindings.button[a]))) {
                        cfg.bindings.cycle(a, lr < 0 ? -1 : 1);
                    }
                }
                if (row(arm64::ActionCount, "Reset Default Controls", nullptr) && (act || ImGui::IsItemClicked())) {
                    cfg.bindings = arm64::Bindings{};
                }
                if (row(arm64::ActionCount + 1, "Back", nullptr) && (act || ImGui::IsItemClicked())) {
                    menu_page = MenuPage::Main; menu_idx = 0;
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("D-Pad: Navigate  |  A: Select/Toggle  |  B: Back  |  Start+Select: Close");
            ImGui::End();
            imgui.render();
            arm64::gl_end_frame();
            SDL_Delay(16);
        }
    }

    save_nvram(*game, data_dir + "/saves");
    cfg.save(settings_path);
    sound_worker.wait();
    sound_worker.close();
    if (audio_ok) audio.close();
    imgui.shutdown();
    gpu.shutdown();
    arm64::gl_fini();
    SDL_Quit();
    return 0;
}
