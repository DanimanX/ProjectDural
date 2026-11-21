// Decoupled Core 2 sound board worker for ProjectDural.
// Runs VF2's 68000 + SCSP + SCSP DSP in parallel on Core 2 via model2a::set_sound_only(true),
// copying ONLY AUDIOCPU (512 KB) and SAMPLES (8 MB) so Core 2 adds ~18 MB RSS instead of 105+ MB.
#pragma once

#include "audio.h"
#include "machine/model2a.h"
#include "thread_affinity.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace arm64 {

inline romset::result clone_sound_roms(const romset::result &full) {
    romset::result r;
    r.ok = full.ok;
    r.regions[romset::MAINCPU].assign(0x2000, 0); // 8 KB dummy for i960_cpu_device::device_reset()
    r.regions[romset::AUDIOCPU] = full.regions[romset::AUDIOCPU];
    r.regions[romset::SAMPLES] = full.regions[romset::SAMPLES];
    return r;
}

class SoundWorker {
public:
    SoundWorker() = default;
    SoundWorker(const SoundWorker &) = delete;
    SoundWorker &operator=(const SoundWorker &) = delete;
    ~SoundWorker() { close(); }

    void set_core(int core) { core_ = core; }

    bool open(const romset::result &full_roms, Audio *audio = nullptr) {
        close();
        audio_ = audio;
        snd_ = std::make_unique<model2a>(clone_sound_roms(full_roms), false);
        snd_->set_sound(true);
        snd_->set_sound_only(true);
        snd_->set_draw_3d(false);
        snd_->reset();
        snd_->scsp().set_dsp_enabled(dsp_enabled_);
        snd_->set_volumes(music_vol_, sfx_vol_);
        stop_ = false;
        busy_ = false;
        pending_reset_ = false;
        queued_bytes_.clear();
        thread_ = std::thread(&SoundWorker::loop, this);
        return true;
    }

    void submit(std::vector<uint8_t> uart_bytes) {
        if (!snd_) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!uart_bytes.empty()) {
                queued_bytes_.insert(queued_bytes_.end(), uart_bytes.begin(), uart_bytes.end());
            }
            busy_ = true;
        }
        cv_.notify_one();
    }

    void wait() {
        if (!thread_.joinable()) return;
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [&] { return !busy_; });
    }

    void reset() {
        if (!snd_) return;
        wait();
        std::lock_guard<std::mutex> lk(mu_);
        queued_bytes_.clear();
        snd_->reset();
        snd_->scsp().set_dsp_enabled(dsp_enabled_);
        snd_->set_volumes(music_vol_, sfx_vol_);
        if (audio_) audio_->pause();
    }

    void set_volumes(float music, float effects) {
        music_vol_ = music;
        sfx_vol_ = effects;
        if (!snd_) return;
        std::lock_guard<std::mutex> lk(mu_);
        pending_vol_ = true;
    }

    void set_dsp_enabled(bool on) {
        dsp_enabled_ = on;
        if (!snd_) return;
        std::lock_guard<std::mutex> lk(mu_);
        pending_dsp_ = true;
    }

    uint64_t samples_total() const { return samples_total_.load(std::memory_order_relaxed); }

    void close() {
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                stop_ = true;
            }
            cv_.notify_all();
            thread_.join();
        }
        snd_.reset();
        audio_ = nullptr;
    }

private:
    void loop() {
        pin_current_thread(core_);
        set_current_thread_name("dural-snd");
        for (;;) {
            std::vector<uint8_t> bytes;
            bool do_vol = false, do_dsp = false;
            float mvol = 1.0f, svol = 1.0f;
            bool dsp = true;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || busy_; });
                if (stop_) return;
                bytes.swap(queued_bytes_);
                do_vol = pending_vol_; pending_vol_ = false;
                do_dsp = pending_dsp_; pending_dsp_ = false;
                mvol = music_vol_; svol = sfx_vol_;
                dsp = dsp_enabled_;
            }
            if (do_vol) snd_->set_volumes(mvol, svol);
            if (do_dsp) snd_->scsp().set_dsp_enabled(dsp);
            if (!bytes.empty()) snd_->sound_bytes(bytes.data(), bytes.size());
            snd_->run_frame();
            if (!snd_->m_audio_out.empty()) {
                samples_total_.fetch_add(snd_->m_audio_out.size() / 2, std::memory_order_relaxed);
                if (audio_) audio_->push(snd_->m_audio_out.data(), snd_->m_audio_out.size());
                snd_->m_audio_out.clear();
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                busy_ = false;
            }
            done_.notify_all();
        }
    }

    int core_ = 2;
    Audio *audio_ = nullptr;
    std::unique_ptr<model2a> snd_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    std::vector<uint8_t> queued_bytes_;
    bool stop_ = false;
    bool busy_ = false;
    bool pending_reset_ = false;
    bool pending_vol_ = false;
    bool pending_dsp_ = false;
    bool dsp_enabled_ = true;
    float music_vol_ = 1.0f;
    float sfx_vol_ = 1.0f;
    std::atomic<uint64_t> samples_total_{0};
};

} // namespace arm64
