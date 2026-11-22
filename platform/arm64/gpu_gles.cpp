// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 renderer for ProjectDural.
#include "gpu_gles.h"
#include "gl_funcs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace arm64 {
namespace {

constexpr size_t kIndexedVertexLimit = 65536;
constexpr size_t kQuadIndices = kIndexedVertexLimit / 4 * 6;

const char *const kVertexShader = R"(
attribute vec3 a_pos;    // x * w, y * w (display pixels, top-left origin), w
attribute vec4 a_uvpd;   // u_norm, v_norm, palette row coord, priority depth [0..1]
attribute vec4 a_color;
uniform vec2 u_inv_screen;
varying highp vec3 v_uvp;
varying mediump vec4 v_color;
void main() {
    float w = a_pos.z;
    float z = (2.0 * a_uvpd.w - 1.0) * w;
    gl_Position = vec4(a_pos.x * (2.0 * u_inv_screen.x) - w,
                       w - a_pos.y * (2.0 * u_inv_screen.y),
                       z, w);
    v_uvp = a_uvpd.xyz;
    v_color = a_color;
}
)";

const char *const kFragmentShader = R"(
varying highp vec3 v_uvp;
varying mediump vec4 v_color;
#ifdef TEXTURED
uniform sampler2D s_tex;
#endif
#ifdef MODEL
uniform sampler2D s_palette;
#endif
void main() {
#ifdef CHECKER
    highp vec2 p = floor(gl_FragCoord.xy);
    highp float s = p.x + p.y;
    if (s - 2.0 * floor(s * 0.5) < 0.5) { discard; }
#endif
#ifdef TEXTURED
    mediump vec4 t = texture2D(s_tex, v_uvp.xy);
#ifdef ALPHA_TEST
    if (t.a < 0.5) { discard; }
#endif
#ifdef MODEL
    mediump float k = floor(t.r * 127.5 + 0.01);
    gl_FragColor = texture2D(s_palette, vec2((k + 0.5) * (1.0 / 128.0), v_uvp.z)) * v_color;
#else
    gl_FragColor = t * v_color;
#endif
#else
    gl_FragColor = v_color;
#endif
}
)";

enum class Prog : uint8_t { Flat, Model, ModelAlpha, FlatChecker, ModelChecker, Ui, Count };
struct ProgramDef { const char *name; const char *defines; };
constexpr ProgramDef kPrograms[int(Prog::Count)] = {
    {"Flat", ""},
    {"Model", "#define TEXTURED\n#define MODEL\n"},
    {"ModelAlpha", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n"},
    {"FlatChecker", "#define CHECKER\n"},
    {"ModelChecker", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n#define CHECKER\n"},
    {"Ui", "#define TEXTURED\n"},
};
inline bool textured(Prog p) { return p != Prog::Flat && p != Prog::FlatChecker; }

struct Vertex {
    float x, y, w;
    float u, v;
    float p;
    float d;
    uint32_t color;
};
static_assert(sizeof(Vertex) == 32, "Vertex must stay 32 bytes");

enum class Depth : uint8_t { Off, TestWrite };

struct Cmd {
    Prog prog;
    GLuint texture;
    uint32_t first, count;
    Depth depth;
    int clip;
    bool blend;
    bool quads;
};

struct PolyRef {
    const gpu3d_poly *poly;
    float depth;
    uint32_t color;
    float palette;
    uint32_t tex_w, tex_h;
};

struct Batch {
    int clip = -1;
    Prog prog = Prog::Flat;
    GLuint texture = 0;
    float max_depth = 0.0f;
    std::vector<PolyRef> poly_refs;
};

struct VertexArena {
    std::vector<Vertex> buf;
    size_t size = 0;
    Vertex *append(size_t n) {
        if (size + n > buf.size()) {
            const size_t cap = std::max<size_t>(buf.empty() ? (size_t(1) << 16) : buf.size() * 2, size + n);
            buf.resize(cap);
        }
        Vertex *p = buf.data() + size;
        size += n;
        return p;
    }
    Vertex *data() { return buf.data(); }
    void release() { buf.clear(); buf.shrink_to_fit(); size = 0; }
};

struct State {
    GlFuncs gl;
    SDL_Window *window = nullptr;
    SDL_GLContext ctx = nullptr;
    bool ready = false;
    bool is_gles = false;
    std::string renderer_str;
    int display_w = 720, display_h = 480;

    GLuint program[int(Prog::Count)] = {};
    GLint u_inv_screen[int(Prog::Count)] = {};
    static constexpr size_t kVboRing = 3;
    GLuint vbo[kVboRing] = {};
    size_t vbo_idx = 0;
    GLuint quad_ibo = 0;
    GLuint palette_texture = 0;

    VertexArena verts;
    std::vector<Cmd> cmds;
    std::vector<std::array<int, 4>> clips;
    int clip = -1;
    std::vector<Batch> batches;
    std::vector<uint32_t> batch_order;
    size_t batch_count = 0;
    static constexpr size_t kBatchHashSize = 2048; // power-of-2 open-addressed hash table
    struct BatchHashSlot { uint64_t key = 0; uint32_t idx = 0; uint32_t gen = 0; };
    BatchHashSlot batch_hash[kBatchHashSize] = {};
    uint32_t batch_gen = 1;

    char error[512] = {};
} g;

void set_error(const char *what, const char *detail = nullptr) {
    std::snprintf(g.error, sizeof g.error, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
    std::fprintf(stderr, "[ProjectDural GL] %s\n", g.error);
}

// Convert 0xAARRGGBB (BGRA bytes on LE) -> 0xAABBGGRR (RGBA bytes for GL_RGBA)
void swizzle_argb_to_rgba(const uint32_t *src, size_t src_stride, uint32_t *dst, int w, int h, bool force_opaque = false) {
#if defined(__aarch64__)
    static const uint8x16_t kMask = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
    const uint32x4_t alpha_or = vdupq_n_u32(force_opaque ? 0xff000000u : 0u);
    for (int y = 0; y < h; ++y) {
        const uint32_t *s = src + size_t(y) * src_stride;
        uint32_t *d = dst + size_t(y) * w;
        int x = 0;
        for (; x + 4 <= w; x += 4) {
            uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(s + x));
            v = vqtbl1q_u8(v, kMask);
            if (force_opaque) v = vreinterpretq_u8_u32(vorrq_u32(vreinterpretq_u32_u8(v), alpha_or));
            vst1q_u8(reinterpret_cast<uint8_t *>(d + x), v);
        }
        for (; x < w; ++x) {
            const uint32_t c = s[x];
            const uint32_t a = force_opaque ? 0xff000000u : (c & 0xff000000u);
            d[x] = a | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
        }
    }
#else
    for (int y = 0; y < h; ++y) {
        const uint32_t *s = src + size_t(y) * src_stride;
        uint32_t *d = dst + size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const uint32_t c = s[x];
            const uint32_t a = force_opaque ? 0xff000000u : (c & 0xff000000u);
            d[x] = a | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
        }
    }
#endif
}

inline uint32_t argb_to_rgba(uint32_t c) {
    return (c & 0xff000000u) | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t(((uint32_t(r) & 0xf8u) << 8) | ((uint32_t(g) & 0xfcu) << 3) | (uint32_t(b) >> 3));
}

void push(Prog prog, GLuint texture, const Vertex *vertices, size_t count, Depth depth = Depth::Off, bool blend = true) {
    if (!count) return;
    const uint32_t first = uint32_t(g.verts.size);
    Vertex *dst = g.verts.append(count);
    std::memcpy(dst, vertices, count * sizeof(Vertex));
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == prog && last.texture == texture && last.depth == depth &&
            last.clip == g.clip && last.blend == blend) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({prog, texture, first, uint32_t(count), depth, g.clip, blend, false});
}

void push_quad(Prog prog, GLuint texture, float x0, float y0, float x1, float y1,
               float u0, float v0, float u1, float v1, uint32_t color,
               Depth depth = Depth::Off, bool blend = true, float d = 0.0f) {
    const Vertex q[6] = {
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y0, 1, u1, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color},
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color}, {x0, y1, 1, u0, v1, 0, d, color},
    };
    push(prog, texture, q, 6, depth, blend);
}

int add_clip(const std::array<int, 4> &rect) {
    for (size_t i = 0; i < g.clips.size(); ++i)
        if (g.clips[i] == rect) return int(i);
    g.clips.push_back(rect);
    return int(g.clips.size() - 1);
}

uint32_t batch_for(int clip, Prog prog, GLuint texture, float depth) {
    const uint64_t key = (uint64_t(uint16_t(clip)) << 40) | (uint64_t(prog) << 32) | texture;
    uint64_t h = key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccduLL;
    size_t slot = size_t(h ^ (h >> 33)) & (State::kBatchHashSize - 1);
    while (g.batch_hash[slot].gen == g.batch_gen) {
        if (g.batch_hash[slot].key == key) {
            const uint32_t idx = g.batch_hash[slot].idx;
            if (depth > g.batches[idx].max_depth) g.batches[idx].max_depth = depth;
            return idx;
        }
        slot = (slot + 1) & (State::kBatchHashSize - 1);
    }
    if (g.batch_count == g.batches.size()) g.batches.emplace_back();
    const uint32_t idx = uint32_t(g.batch_count++);
    Batch &b = g.batches[idx];
    b.clip = clip;
    b.prog = prog;
    b.texture = texture;
    b.max_depth = depth;
    b.poly_refs.clear();
    g.batch_hash[slot] = {key, idx, g.batch_gen};
    return idx;
}

Vertex *append_quads(size_t quads) {
    if (!g.quad_ibo || !quads) return nullptr;
    const size_t pad = (4 - g.verts.size % 4) % 4;
    if (g.verts.size + pad + quads * 4 > kIndexedVertexLimit) return nullptr;
    g.verts.append(pad + quads * 4);
    return g.verts.data() + (g.verts.size - quads * 4);
}

size_t write_fan_quads(Vertex *dst, const Vertex *v, int n) {
    size_t written = 0;
    for (int a = 1; a + 1 < n; a += 2) {
        *dst++ = v[0];
        *dst++ = v[a];
        *dst++ = v[a + 1];
        *dst++ = v[std::min(a + 2, n - 1)];
        written += 4;
    }
    return written;
}

void execute() {
    if (g.cmds.empty() || !g.verts.size) return;
    auto &gl = g.gl;
    const GLuint cur_vbo = g.vbo[g.vbo_idx];
    g.vbo_idx = (g.vbo_idx + 1) % State::kVboRing;
    gl.glBindBuffer(GL_ARRAY_BUFFER, cur_vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.verts.size * sizeof(Vertex)), nullptr, GL_STREAM_DRAW);
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.verts.size * sizeof(Vertex)), g.verts.data(), GL_STREAM_DRAW);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
    for (GLuint i = 0; i < 3; ++i) gl.glEnableVertexAttribArray(i);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(0));
    gl.glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(12));
    gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(28));

    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, g.palette_texture);
    gl.glActiveTexture(GL_TEXTURE0);

    const float inv_w = 1.0f / float(std::max(g.display_w, 1));
    const float inv_h = 1.0f / float(std::max(g.display_h, 1));

    GLuint program = 0, texture = 0;
    int blend = -1, depth_test = -1, clip = -2;
    for (const Cmd &c : g.cmds) {
        const int pi = int(c.prog);
        if (g.program[pi] != program) {
            program = g.program[pi];
            gl.glUseProgram(program);
            if (g.u_inv_screen[pi] >= 0) gl.glUniform2f(g.u_inv_screen[pi], inv_w, inv_h);
        }
        const int want_blend = c.blend ? 1 : 0;
        if (want_blend != blend) {
            if (want_blend) gl.glEnable(GL_BLEND);
            else gl.glDisable(GL_BLEND);
            blend = want_blend;
        }
        const int want_test = c.depth != Depth::Off ? 1 : 0;
        if (want_test != depth_test) {
            if (want_test) gl.glEnable(GL_DEPTH_TEST);
            else gl.glDisable(GL_DEPTH_TEST);
            depth_test = want_test;
        }
        if (textured(c.prog) && c.texture != texture) {
            gl.glBindTexture(GL_TEXTURE_2D, c.texture);
            texture = c.texture;
        }
        if (c.clip != clip) {
            if (c.clip < 0) {
                gl.glDisable(GL_SCISSOR_TEST);
            } else {
                const std::array<int, 4> &r = g.clips[size_t(c.clip)];
                gl.glEnable(GL_SCISSOR_TEST);
                gl.glScissor(r[0], g.display_h - r[3], r[2] - r[0], r[3] - r[1]);
            }
            clip = c.clip;
        }
        if (c.quads) {
            const size_t offset = size_t(c.first / 4) * 6 * sizeof(uint16_t);
            gl.glDrawElements(GL_TRIANGLES, GLsizei(c.count / 4 * 6), GL_UNSIGNED_SHORT,
                              reinterpret_cast<const void *>(offset));
        } else {
            gl.glDrawArrays(GL_TRIANGLES, GLint(c.first), GLsizei(c.count));
        }
    }
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_DEPTH_TEST);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLuint compile_shader(GLenum type, const char *header, const char *defines, const char *body, const char *name) {
    auto &gl = g.gl;
    const GLuint shader = gl.glCreateShader(type);
    const GLchar *sources[] = {header, defines, body};
    gl.glShaderSource(shader, 3, sources, nullptr);
    gl.glCompileShader(shader);
    GLint ok = GL_FALSE;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[400] = {};
    gl.glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    set_error(name, log);
    gl.glDeleteShader(shader);
    return 0;
}

GLuint build_program(const ProgramDef &def, GLint &u_inv_out) {
    const char *v_hdr = g.is_gles ? "#version 100\nprecision highp float;\n"
                                  : "#version 120\n#define highp\n#define mediump\n#define lowp\n";
    const char *f_hdr = g.is_gles ? "#version 100\nprecision mediump float;\n"
                                  : "#version 120\n#define highp\n#define mediump\n#define lowp\n";
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, v_hdr, def.defines, kVertexShader, def.name);
    if (!vs) return 0;
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, f_hdr, def.defines, kFragmentShader, def.name);
    if (!fs) { g.gl.glDeleteShader(vs); return 0; }
    auto &gl = g.gl;
    const GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs);
    gl.glAttachShader(prog, fs);
    gl.glBindAttribLocation(prog, 0, "a_pos");
    gl.glBindAttribLocation(prog, 1, "a_uvpd");
    gl.glBindAttribLocation(prog, 2, "a_color");
    gl.glLinkProgram(prog);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    GLint ok = GL_FALSE;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[400] = {};
        gl.glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        set_error(def.name, log);
        gl.glDeleteProgram(prog);
        return 0;
    }
    gl.glUseProgram(prog);
    u_inv_out = gl.glGetUniformLocation(prog, "u_inv_screen");
    const GLint s_tex = gl.glGetUniformLocation(prog, "s_tex");
    if (s_tex >= 0) gl.glUniform1i(s_tex, 0);
    const GLint s_pal = gl.glGetUniformLocation(prog, "s_palette");
    if (s_pal >= 0) gl.glUniform1i(s_pal, 1);
    return prog;
}

void compute_viewport(int w2d, AspectMode aspect, float &sx, float &sy, float &ox, float &oy) {
    const float dw = float(std::max(g.display_w, 1));
    const float dh = float(std::max(g.display_h, 1));
    const float sw = float(std::max(w2d, 1));
    const float sh = 384.0f;
    if (aspect == AspectMode::StretchFull) {
        sx = dw / sw;
        sy = dh / sh;
        ox = 0.0f;
        oy = 0.0f;
    } else {
        const float s = std::min(dw / sw, dh / sh);
        sx = s;
        sy = s;
        ox = (dw - sw * s) * 0.5f;
        oy = (dh - sh * s) * 0.5f;
    }
}

// Exact Model 2 texture sheet swizzle (model2_renderer::get_texel)
inline uint32_t get_texel(uint32_t base_x, uint32_t base_y, uint32_t x, uint32_t y, const uint32_t *sheet) {
    uint32_t x2 = base_x + x;
    uint32_t y2 = base_y + y;
    const uint32_t right = x2 >= 1024 ? 1024u : 0u;
    x2 -= right;
    y2 ^= right;
    const uint32_t offset = ((y2 >> 1) << 9) + (x2 >> 1);
    const uint32_t texel = sheet[offset >> 1];
    const uint32_t shift = ((offset & 1) << 4) + ((~y & 1u) << 3) + ((~x & 1u) << 2);
    return (texel >> shift) & 0x0f;
}

inline uint64_t hash_tex_rect(uint32_t base_x, uint32_t base_y, uint32_t w, uint32_t h, const uint32_t *sheet) {
    uint64_t hval = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y += 2) {
        const uint32_t ry = base_y + y;
        for (uint32_t x = 0; x < w; x += 4) {
            uint32_t x2 = base_x + x;
            uint32_t y2 = ry;
            const uint32_t right = x2 >= 1024 ? 1024u : 0u;
            x2 -= right;
            y2 ^= right;
            const uint32_t offset = ((y2 >> 1) << 9) + (x2 >> 1);
            hval ^= sheet[offset >> 1];
            hval *= 0x100000001b3ull;
        }
    }
    return hval;
}

} // namespace

bool gl_init(const GlConfig &cfg) {
    gl_fini();
    g.error[0] = 0;

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    struct TryProfile { int profile; int maj; int min; int depth; };
    const TryProfile profiles[] = {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, 24},
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, 16},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, 24},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, 16},
    };
    for (const auto &p : profiles) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, p.depth);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, p.profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, p.maj);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, p.min);
        g.window = SDL_CreateWindow("ProjectDural — Virtua Fighter 2", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, cfg.window_w, cfg.window_h, flags);
        if (!g.window) continue;
        g.ctx = SDL_GL_CreateContext(g.window);
        if (g.ctx) break;
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    if (!g.window || !g.ctx) {
        set_error("SDL_GL_CreateContext failed", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g.window, g.ctx);
    if (!g.gl.load()) {
        set_error("Failed to load GLES 2.0 / GL 2.1 functions");
        gl_fini();
        return false;
    }
    const char *ver = reinterpret_cast<const char *>(g.gl.glGetString(GL_VERSION));
    const char *ren = reinterpret_cast<const char *>(g.gl.glGetString(GL_RENDERER));
    g.is_gles = (ver && std::strstr(ver, "OpenGL ES") != nullptr);
    g.renderer_str = std::string(g.is_gles ? "GLES2: " : "GL2: ") + (ren ? ren : "unknown") +
                     " (" + (ver ? ver : "?") + ")";
    std::fprintf(stderr, "[ProjectDural] Graphics: %s\n", g.renderer_str.c_str());

    SDL_ShowCursor(SDL_DISABLE);
    gl_set_vsync(cfg.vsync);
    gl_update_drawable_size();

    auto &gl = g.gl;
    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear_depth(0.0f);
    gl.glDepthFunc(GL_GREATER);
    gl.depth_range(0.0f, 1.0f);
    gl.glDepthMask(GL_TRUE);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < int(Prog::Count); ++i)
        g.program[i] = build_program(kPrograms[i], g.u_inv_screen[i]);
    if (!g.program[int(Prog::Flat)] || !g.program[int(Prog::Model)] ||
        !g.program[int(Prog::ModelAlpha)] || !g.program[int(Prog::Ui)]) {
        gl_fini();
        return false;
    }
    if (!g.program[int(Prog::FlatChecker)] || !g.program[int(Prog::ModelChecker)]) {
        if (g.program[int(Prog::FlatChecker)]) gl.glDeleteProgram(g.program[int(Prog::FlatChecker)]);
        if (g.program[int(Prog::ModelChecker)]) gl.glDeleteProgram(g.program[int(Prog::ModelChecker)]);
        g.program[int(Prog::FlatChecker)] = g.program[int(Prog::Flat)];
        g.u_inv_screen[int(Prog::FlatChecker)] = g.u_inv_screen[int(Prog::Flat)];
        g.program[int(Prog::ModelChecker)] = g.program[int(Prog::ModelAlpha)];
        g.u_inv_screen[int(Prog::ModelChecker)] = g.u_inv_screen[int(Prog::ModelAlpha)];
    }

    gl.glGenBuffers(State::kVboRing, g.vbo);
    if (!g.vbo[0]) {
        set_error("VBO creation failed");
        gl_fini();
        return false;
    }
    g.verts.buf.resize(size_t(1) << 16);
    g.cmds.reserve(1024);

    std::vector<uint16_t> indices(kQuadIndices);
    for (size_t q = 0; q < kIndexedVertexLimit / 4; ++q) {
        const uint16_t v = uint16_t(q * 4);
        uint16_t *idx = &indices[q * 6];
        idx[0] = v; idx[1] = uint16_t(v + 1); idx[2] = uint16_t(v + 2);
        idx[3] = v; idx[4] = uint16_t(v + 2); idx[5] = uint16_t(v + 3);
    }
    gl.glGenBuffers(1, &g.quad_ibo);
    if (g.quad_ibo) {
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(uint16_t)), indices.data(),
                        GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    g.ready = true;
    return true;
}

const char *gl_error() { return g.error; }
const char *gl_renderer_string() { return g.renderer_str.c_str(); }
bool gl_is_gles() { return g.is_gles; }
SDL_Window *gl_window() { return g.window; }
int gl_display_width() { return g.display_w; }
int gl_display_height() { return g.display_h; }

void gl_update_drawable_size() {
    if (!g.window) return;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g.window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(g.window, &w, &h);
    g.display_w = std::max(w, 1);
    g.display_h = std::max(h, 1);
}

void gl_set_vsync(bool enabled) {
    if (SDL_GL_SetSwapInterval(enabled ? 1 : 0) < 0 && enabled)
        SDL_GL_SetSwapInterval(0);
}

void gl_begin_frame() {
    g.verts.size = 0;
    g.cmds.clear();
    g.clips.clear();
    g.clip = -1;
    auto &gl = g.gl;
    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDepthMask(GL_TRUE);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_end_frame() {
    execute();
    if (g.window) SDL_GL_SwapWindow(g.window);
}

void gl_fini() {
    if (g.ctx) {
        auto &gl = g.gl;
        if (gl.glFinish) gl.glFinish();
        for (int i = 0; i < int(Prog::Count); ++i) {
            bool shared = false;
            for (int j = 0; j < i; ++j) shared = shared || g.program[j] == g.program[i];
            if (g.program[i] && !shared && gl.glDeleteProgram) gl.glDeleteProgram(g.program[i]);
            g.program[i] = 0;
        }
        if (g.vbo[0] && gl.glDeleteBuffers) gl.glDeleteBuffers(State::kVboRing, g.vbo);
        if (g.quad_ibo && gl.glDeleteBuffers) gl.glDeleteBuffers(1, &g.quad_ibo);
        for (auto &v : g.vbo) v = 0;
        g.quad_ibo = 0;
        SDL_GL_DeleteContext(g.ctx);
        g.ctx = nullptr;
    }
    if (g.window) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    g.verts.release();
    g.ready = false;
}

void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba) {
    push_quad(Prog::Flat, 0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, rgba, Depth::Off, true);
}

uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    if (w <= 0 || h <= 0 || !rgba || !g.ready) return 0;
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

void gl_delete_ui_texture(uint32_t texture) {
    if (!texture || !g.ready) return;
    GLuint t = texture;
    g.gl.glDeleteTextures(1, &t);
}

void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1) {
    count -= count % 3;
    if (!texture || !count) return;
    x0 = std::clamp(x0, 0, g.display_w);
    x1 = std::clamp(x1, 0, g.display_w);
    y0 = std::clamp(y0, 0, g.display_h);
    y1 = std::clamp(y1, 0, g.display_h);
    if (x1 <= x0 || y1 <= y0) return;
    Vertex *dst = g.verts.append(count);
    const uint32_t first = uint32_t(dst - g.verts.data());
    for (size_t i = 0; i < count; ++i) {
        const GlUiVertex &v = vertices[i];
        dst[i] = {v.x, v.y, 1.0f, v.u, v.v, 0.0f, 0.0f, v.rgba};
    }
    const int clip = add_clip({x0, y0, x1, y1});
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == Prog::Ui && last.texture == texture && last.clip == clip &&
            last.blend && last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({Prog::Ui, texture, first, uint32_t(count), Depth::Off, clip, true, false});
}

// ---- GpuGlesRenderer ---------------------------------------------------------

GpuGlesRenderer::GpuGlesRenderer() {
    palette_shadow_.assign(size_t(kPaletteWidth) * kPaletteRows, 0u);
    auto &gl = g.gl;
    GLuint ptex = 0;
    if (g.ready) {
        gl.glGenTextures(1, &ptex);
        if (ptex) {
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, ptex);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kPaletteWidth, kPaletteRows, 0,
                            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, palette_shadow_.data());
        }
    }
    palette_texture_ = ptex;
    g.palette_texture = palette_texture_;
    sources_.reserve(1024);
    ok_ = g.ready && palette_texture_ != 0;
}

GpuGlesRenderer::~GpuGlesRenderer() { shutdown(); }

uint32_t GpuGlesRenderer::make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                                       uint32_t format, const void *pixels) {
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap_s));
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap_t));
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), GLsizei(w), GLsizei(h), 0, GLenum(format),
                    GL_UNSIGNED_BYTE, pixels);
    return tex;
}

void GpuGlesRenderer::update_texture_rgba(uint32_t &tex, int &cur_w, int &cur_h, int w, int h, bool linear,
                                          const uint32_t *argb, size_t stride_px) {
    if (w <= 0 || h <= 0 || !argb) return;
    swizzle_scratch_.resize(size_t(w) * size_t(h));
    swizzle_argb_to_rgba(argb, stride_px, swizzle_scratch_.data(), w, h, false);
    auto &gl = g.gl;
    if (!tex || cur_w != w || cur_h != h) {
        if (tex && g.ready) { GLuint t = tex; gl.glDeleteTextures(1, &t); }
        tex = make_texture(uint32_t(w), uint32_t(h), linear, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE,
                           GL_RGBA, swizzle_scratch_.data());
        cur_w = w;
        cur_h = h;
    } else {
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D, tex);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, swizzle_scratch_.data());
    }
}

void GpuGlesRenderer::update_texture_2d_rgb565(LayerTex2D &layer, int w, int h, bool linear,
                                               const uint32_t *argb, size_t stride_px) {
    if (w <= 0 || h <= 0 || !argb) return;
    auto &gl = g.gl;
    const size_t total = size_t(w) * size_t(h);
    const bool realloc_tex = (!layer.tex[0] || !layer.tex[1] || layer.w != w || layer.h != h ||
                              layer.shadow[0].size() != total || layer.shadow[1].size() != total);
    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            if (layer.tex[i] && g.ready) { GLuint t = layer.tex[i]; gl.glDeleteTextures(1, &t); layer.tex[i] = 0; }
            layer.shadow[i].assign(total, 0u);
        }
        layer.w = w;
        layer.h = h;
        layer.linear = linear;
        layer.cur = 0;
    } else {
        layer.cur ^= 1u;
    }

    uint32_t &tex = layer.tex[layer.cur];
    std::vector<uint16_t> &shadow = layer.shadow[layer.cur];
    uint8_t row_dirty[512] = {};

    for (int y = 0; y < h; ++y) {
        const uint32_t *src = argb + size_t(y) * stride_px;
        uint16_t *dst = shadow.data() + size_t(y) * size_t(w);
        uint32_t diff_acc = realloc_tex ? 1u : 0u;
        int x = 0;
#if defined(__aarch64__)
        uint16x8_t vdiff = vdupq_n_u16(0);
        for (; x + 8 <= w; x += 8) {
            const uint32x4_t c0 = vld1q_u32(src + x);
            const uint32x4_t c1 = vld1q_u32(src + x + 4);
            const uint16x4_t p0 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c0, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c0, 5), vdup_n_u16(0x07e0u))),
                vand_u16(vshr_n_u16(vmovn_u32(c0), 3), vdup_n_u16(0x001fu)));
            const uint16x4_t p1 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c1, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c1, 5), vdup_n_u16(0x07e0u))),
                vand_u16(vshr_n_u16(vmovn_u32(c1), 3), vdup_n_u16(0x001fu)));
            const uint16x8_t p = vcombine_u16(p0, p1);
            const uint16x8_t old = vld1q_u16(dst + x);
            vdiff = vorrq_u16(vdiff, veorq_u16(old, p));
            vst1q_u16(dst + x, p);
        }
        diff_acc |= vmaxvq_u16(vdiff);
#endif
        for (; x < w; ++x) {
            const uint32_t c = src[x];
            const uint16_t p = uint16_t(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu));
            diff_acc |= uint32_t(dst[x] ^ p);
            dst[x] = p;
        }
        if (diff_acc && y < 512) row_dirty[y] = 1;
    }

    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            GLuint t = 0;
            gl.glGenTextures(1, &t);
            layer.tex[i] = t;
            if (!t) continue;
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, t);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, shadow.data());
        }
        if (layer.shadow[1].data() != shadow.data()) layer.shadow[1] = shadow;
        return;
    }

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    if (layer.linear != linear) {
        for (int i = 0; i < 2; ++i) {
            gl.glBindTexture(GL_TEXTURE_2D, layer.tex[i]);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        }
        gl.glBindTexture(GL_TEXTURE_2D, tex);
        layer.linear = linear;
    }

    int y = 0;
    while (y < h) {
        while (y < h && !row_dirty[y]) ++y;
        if (y >= h) break;
        const int y0 = y;
        int y1 = y + 1;
        while (y1 < h) {
            if (row_dirty[y1]) { ++y1; continue; }
            int gap = 0;
            while (y1 + gap < h && !row_dirty[y1 + gap] && gap < 8) ++gap;
            if (y1 + gap < h && row_dirty[y1 + gap]) { y1 += gap + 1; continue; }
            break;
        }
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                           shadow.data() + size_t(y0) * size_t(w));
        y = y1;
    }
}

void GpuGlesRenderer::update_texture_2d_rgba5551(LayerTex2D &layer, int w, int h, bool linear,
                                                 const uint32_t *argb, size_t stride_px,
                                                 int y_active0, int y_active1, int &prev_y0, int &prev_y1) {
    if (w <= 0 || h <= 0 || !argb) return;
    auto &gl = g.gl;
    const size_t total = size_t(w) * size_t(h);
    const bool realloc_tex = (!layer.tex[0] || !layer.tex[1] || layer.w != w || layer.h != h ||
                              layer.shadow[0].size() != total || layer.shadow[1].size() != total);
    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            if (layer.tex[i] && g.ready) { GLuint t = layer.tex[i]; gl.glDeleteTextures(1, &t); layer.tex[i] = 0; }
            layer.shadow[i].assign(total, 0u);
        }
        layer.w = w;
        layer.h = h;
        layer.linear = linear;
        layer.cur = 0;
        prev_y0 = 0;
        prev_y1 = h;
    } else {
        layer.cur ^= 1u;
    }

    uint32_t &tex = layer.tex[layer.cur];
    std::vector<uint16_t> &shadow = layer.shadow[layer.cur];

    const int scan_y0 = realloc_tex ? 0 : std::clamp(std::min(y_active0, prev_y0), 0, h);
    const int scan_y1 = realloc_tex ? h : std::clamp(std::max(y_active1, prev_y1), 0, h);
    prev_y0 = std::clamp(y_active0, 0, h);
    prev_y1 = std::clamp(y_active1, 0, h);

    uint8_t row_dirty[512] = {};
    for (int y = scan_y0; y < scan_y1; ++y) {
        const uint32_t *src = argb + size_t(y) * stride_px;
        uint16_t *dst = shadow.data() + size_t(y) * size_t(w);
        uint32_t diff_acc = realloc_tex ? 1u : 0u;
        int x = 0;
#if defined(__aarch64__)
        uint16x8_t vdiff = vdupq_n_u16(0);
        const uint32x4_t vzero32 = vdupq_n_u32(0);
        for (; x + 8 <= w; x += 8) {
            const uint32x4_t c0 = vld1q_u32(src + x);
            const uint32x4_t c1 = vld1q_u32(src + x + 4);
            const uint16x4_t m0 = vmovn_u32(vcgtq_u32(c0, vzero32));
            const uint16x4_t m1 = vmovn_u32(vcgtq_u32(c1, vzero32));
            const uint16x4_t rgb0 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c0, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c0, 5), vdup_n_u16(0x07c0u))),
                vorr_u16(vand_u16(vshrn_n_u32(c0, 2), vdup_n_u16(0x003eu)), vdup_n_u16(1u)));
            const uint16x4_t rgb1 = vorr_u16(
                vorr_u16(vand_u16(vshrn_n_u32(c1, 8), vdup_n_u16(0xf800u)),
                         vand_u16(vshrn_n_u32(c1, 5), vdup_n_u16(0x07c0u))),
                vorr_u16(vand_u16(vshrn_n_u32(c1, 2), vdup_n_u16(0x003eu)), vdup_n_u16(1u)));
            const uint16x8_t p = vcombine_u16(vand_u16(rgb0, m0), vand_u16(rgb1, m1));
            const uint16x8_t old = vld1q_u16(dst + x);
            vdiff = vorrq_u16(vdiff, veorq_u16(old, p));
            vst1q_u16(dst + x, p);
        }
        diff_acc |= vmaxvq_u16(vdiff);
#endif
        for (; x < w; ++x) {
            const uint32_t c = src[x];
            const uint16_t p = (c >> 24)
                ? uint16_t((((c >> 19) & 0x1fu) << 11) | (((c >> 11) & 0x1fu) << 6) | (((c >> 3) & 0x1fu) << 1) | 1u)
                : 0u;
            diff_acc |= uint32_t(dst[x] ^ p);
            dst[x] = p;
        }
        if (diff_acc && y < 512) row_dirty[y] = 1;
    }

    if (realloc_tex) {
        for (int i = 0; i < 2; ++i) {
            GLuint t = 0;
            gl.glGenTextures(1, &t);
            layer.tex[i] = t;
            if (!t) continue;
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, t);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, shadow.data());
        }
        if (layer.shadow[1].data() != shadow.data()) layer.shadow[1] = shadow;
        return;
    }

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    int y = scan_y0;
    while (y < scan_y1) {
        while (y < scan_y1 && !row_dirty[y]) ++y;
        if (y >= scan_y1) break;
        const int y0 = y;
        int y1 = y + 1;
        while (y1 < scan_y1) {
            if (row_dirty[y1]) { ++y1; continue; }
            int gap = 0;
            while (y1 + gap < scan_y1 && !row_dirty[y1 + gap] && gap < 8) ++gap;
            if (y1 + gap < scan_y1 && row_dirty[y1 + gap]) { y1 += gap + 1; continue; }
            break;
        }
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1,
                           shadow.data() + size_t(y0) * size_t(w));
        y = y1;
    }
}

void GpuGlesRenderer::clear_tex_cache() {
    auto &gl = g.gl;
    for (auto &entry : sources_) {
        GLuint tex = entry.second.texture;
        if (tex && g.ready) gl.glDeleteTextures(1, &tex);
    }
    sources_.clear();
    cached_bytes_ = 0;
    last_tex_clears_++;
}

void GpuGlesRenderer::clear_tex_sheet(uint32_t sheet) {
    const uint64_t s = sheet & 1u;
    for (auto &entry : sources_) {
        if ((entry.first.val & 1u) == s) {
            entry.second.stale = true;
        }
    }
    last_tex_clears_++;
}

void GpuGlesRenderer::clear_pal_cache() {
    palette_index_.clear();
    palette_used_ = 0;
    palette_dirty_begin_ = UINT32_MAX;
    palette_dirty_end_ = 0;
    last_pal_clears_++;
}

void GpuGlesRenderer::reset_caches() {
    clear_tex_cache();
    clear_pal_cache();
    last_2d_version_ = UINT64_MAX;
    last_sides_version_ = UINT64_MAX;
    last_tex_gen_[0] = last_tex_gen_[1] = UINT32_MAX;
    last_luma_gen_ = last_xlat_gen_ = UINT32_MAX;
    have_gamma_ = false;
}

void GpuGlesRenderer::shutdown() {
    if (shutdown_) return;
    clear_tex_cache();
    clear_pal_cache();
    auto del = [&](uint32_t &t) {
        if (t && g.ready) { GLuint tt = t; g.gl.glDeleteTextures(1, &tt); }
        t = 0;
    };
    auto del_layer = [&](LayerTex2D &l) {
        del(l.tex[0]);
        del(l.tex[1]);
        l.w = l.h = 0;
    };
    del(palette_texture_);
    del_layer(layer_bd_);
    del_layer(layer_mid_);
    del_layer(layer_top_);
    del(tex_cpu3d_);
    del(tex_sides_);
    g.palette_texture = 0;
    shutdown_ = true;
    ok_ = false;
}

const GpuGlesRenderer::TexSource *GpuGlesRenderer::source_for(const gpu3d_poly &poly, const gpu3d_rams &rams) {
    const uint32_t w = std::max<uint32_t>(poly.texw, 32);
    const uint32_t h = std::max<uint32_t>(poly.texh, 32);
    const bool trans = (poly.renderer & 1) != 0;
    const uint64_t key_val =
        uint64_t(poly.sheet & 1) |
        (uint64_t(poly.texx & 0x7ffu) << 1) |
        (uint64_t(poly.texy & 0x3ffu) << 12) |
        (uint64_t(w & 0x7ffu) << 22) |
        (uint64_t(h & 0x7ffu) << 33) |
        (uint64_t(trans ? 1 : 0) << 44) |
        (uint64_t(poly.wrapx & 1) << 45) |
        (uint64_t(poly.wrapy & 1) << 46) |
        (uint64_t(poly.mirrorx & 1) << 47) |
        (uint64_t(poly.mirrory & 1) << 48);
    const TexKey key{key_val};
    const uint32_t *sheet = poly.sheet ? rams.tex[1] : rams.tex[0];
    if (!sheet) return nullptr;

    auto found = sources_.find(key);
    uint64_t rect_hash = 0;
    bool have_hash = false;
    if (found != sources_.end()) {
        if (!found->second.stale) return &found->second;
        rect_hash = hash_tex_rect(poly.texx, poly.texy, w, h, sheet);
        have_hash = true;
        if (rect_hash == found->second.hash) {
            found->second.stale = false;
            return &found->second;
        }
    }

    if (found == sources_.end() && cached_bytes_ >= kSourceCacheBytes) clear_tex_cache();

    const uint32_t step_x = std::max(1u, (w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (h + kTextureLimit - 1) / kTextureLimit);
    const uint32_t tw = (w + step_x - 1) / step_x;
    const uint32_t th = (h + step_y - 1) / step_y;

    index_scratch_.resize(size_t(tw) * size_t(th) * 2u);
    uint8_t *la = index_scratch_.data();
    const uint8_t trans_alpha = trans ? 0 : 255;
    for (uint32_t y = 0; y < th; ++y) {
        const uint32_t ty = std::min(y * step_y, h - 1);
        uint8_t *row = la + size_t(y) * tw * 2u;
        for (uint32_t x = 0; x < tw; ++x) {
            const uint32_t tx = x * step_x;
            const uint32_t idx = get_texel(poly.texx, poly.texy, tx, ty, sheet);
            row[x * 2u] = uint8_t(idx * 16);
            row[x * 2u + 1] = idx == 15 ? trans_alpha : 255;
        }
    }
    if (trans) {
        for (uint32_t y = 0; y < th; ++y) {
            for (uint32_t x = 0; x < tw; ++x) {
                const size_t i = (size_t(y) * tw + x) * 2u;
                if (la[i + 1]) continue;
                const size_t n[4] = {
                    (size_t(y) * tw + (x + 1) % tw) * 2u,
                    (size_t(y) * tw + (x + tw - 1) % tw) * 2u,
                    (size_t((y + 1) % th) * tw + x) * 2u,
                    (size_t((y + th - 1) % th) * tw + x) * 2u
                };
                for (size_t j : n) {
                    if (la[j + 1]) { la[i] = la[j]; break; }
                }
            }
        }
    }
    if (!have_hash) rect_hash = hash_tex_rect(poly.texx, poly.texy, w, h, sheet);
    if (found != sources_.end()) {
        auto &gl = g.gl;
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D, found->second.texture);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(tw), GLsizei(th), GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
        found->second.hash = rect_hash;
        found->second.stale = false;
        return &found->second;
    }
    const uint32_t wrap_s = poly.mirrorx ? GL_MIRRORED_REPEAT : GL_REPEAT;
    const uint32_t wrap_t = poly.mirrory ? GL_MIRRORED_REPEAT : GL_REPEAT;
    TexSource s;
    s.w = w;
    s.h = h;
    s.transparent = trans;
    s.stale = false;
    s.hash = rect_hash;
    s.texture = make_texture(tw, th, true, wrap_s, wrap_t, GL_LUMINANCE_ALPHA, la);
    if (!s.texture) return nullptr;
    s.bytes = index_scratch_.size();
    cached_bytes_ += s.bytes;
    return &sources_.emplace(key, s).first->second;
}

int GpuGlesRenderer::palette_row(const gpu3d_poly &poly, const gpu3d_rams &rams) {
    const uint32_t lumabase = poly.lumabase & 0x7fffu;
    const uint32_t pal = poly.pal & 0x7fffu;
    const uint32_t luma_scale = poly.luma & 0xfffcu;
    const uint64_t key = uint64_t(lumabase) | (uint64_t(pal) << 16) | (uint64_t(luma_scale) << 32);
    auto it = palette_index_.find(key);
    if (it != palette_index_.end()) return int(it->second);
    if (palette_used_ >= kPaletteRows) clear_pal_cache();

    const uint32_t row = palette_used_++;
    uint16_t *out = &palette_shadow_[size_t(row) * kPaletteWidth];
    const uint16_t *xr = &rams.xlat[0x0000 / 2 + (((pal >> 0) & 0x1f) << 8)];
    const uint16_t *xg = &rams.xlat[0x4000 / 2 + (((pal >> 5) & 0x1f) << 8)];
    const uint16_t *xb = &rams.xlat[0x8000 / 2 + (((pal >> 10) & 0x1f) << 8)];
    for (uint32_t k = 0; k < kPaletteWidth; ++k) {
        const uint32_t luma = std::min((uint32_t(rams.luma[(lumabase + k) & 0x7fffu]) * luma_scale) >> 8, 0x3fu);
        const uint8_t r = rams.gamma[xr[luma] & 0xff];
        const uint8_t gg = rams.gamma[xg[luma] & 0xff];
        const uint8_t b = rams.gamma[xb[luma] & 0xff];
        out[k] = pack_rgb565(r, gg, b);
    }
    palette_index_[key] = row;
    palette_dirty_begin_ = std::min(palette_dirty_begin_, row);
    palette_dirty_end_ = std::max(palette_dirty_end_, row + 1);
    return int(row);
}

void GpuGlesRenderer::flush_palette_gl() {
    if (palette_dirty_end_ <= palette_dirty_begin_ || !palette_texture_) return;
    auto &gl = g.gl;
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, palette_texture_);
    gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(palette_dirty_begin_), GLsizei(kPaletteWidth),
                       GLsizei(palette_dirty_end_ - palette_dirty_begin_), GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                       &palette_shadow_[size_t(palette_dirty_begin_) * kPaletteWidth]);
    gl.glActiveTexture(GL_TEXTURE0);
    palette_dirty_begin_ = UINT32_MAX;
    palette_dirty_end_ = 0;
}

void GpuGlesRenderer::emit_3d_polys(const gpu3d_poly *polys, size_t count, const gpu3d_rams &rams,
                                    int w2d, float sx, float sy, float ox, float oy) {
    last_poly_count_ = count;
    last_batch_count_ = 0;
    if (!count || !polys) return;

    if (rams.gen[0] != last_tex_gen_[0] || rams.gen[1] != last_tex_gen_[1]) {
        const bool ch0 = rams.gen[0] != last_tex_gen_[0];
        const bool ch1 = rams.gen[1] != last_tex_gen_[1];
        if (last_tex_gen_[0] == UINT32_MAX) clear_tex_cache();
        else {
            if (ch0) clear_tex_sheet(0);
            if (ch1) clear_tex_sheet(1);
        }
        last_tex_gen_[0] = rams.gen[0];
        last_tex_gen_[1] = rams.gen[1];
    }
    if (rams.gen[2] != last_luma_gen_ || rams.gen[3] != last_xlat_gen_ || !have_gamma_ ||
        (rams.gamma && std::memcmp(last_gamma_, rams.gamma, 256) != 0)) {
        clear_pal_cache();
        last_luma_gen_ = rams.gen[2];
        last_xlat_gen_ = rams.gen[3];
        if (rams.gamma) {
            std::memcpy(last_gamma_, rams.gamma, 256);
            have_gamma_ = true;
        }
    }

    g.batch_count = 0;
    if (++g.batch_gen == 0) {
        std::memset(g.batch_hash, 0, sizeof(g.batch_hash));
        g.batch_gen = 1;
    }

    const float depth_step = 0.9998f / float(count + 2);
    for (size_t i = 0; i < count; ++i) {
        const gpu3d_poly &p = polys[i];
        if (p.n < 3 || p.n > 8 || p.renderer == 1) continue;

        const int vx0 = std::max(0, int(p.vp[0]));
        const int vx1 = std::min(w2d - 1, int(p.vp[1]));
        const int vy0 = std::max(0, int(p.vp[2]));
        const int vy1 = std::min(383, int(p.vp[3]));
        if (vx1 < vx0 || vy1 < vy0) continue;

        const int sx0 = std::clamp(int(std::floor(ox + float(vx0) * sx)), 0, g.display_w);
        const int sy0 = std::clamp(int(std::floor(oy + float(vy0) * sy)), 0, g.display_h);
        const int sx1 = std::clamp(int(std::ceil(ox + float(vx1 + 1) * sx)), 0, g.display_w);
        const int sy1 = std::clamp(int(std::ceil(oy + float(vy1 + 1) * sy)), 0, g.display_h);
        if (sx1 <= sx0 || sy1 <= sy0) continue;
        const int clip = add_clip({sx0, sy0, sx1, sy1});

        const float depth = 0.9999f - float(i + 1) * depth_step;
        if (!(p.renderer & 2)) {
            const Prog prog = p.checker ? Prog::FlatChecker : Prog::Flat;
            const uint32_t bi = batch_for(clip, prog, 0, depth);
            g.batches[bi].poly_refs.push_back({&p, depth, argb_to_rgba(p.colour), 0.0f, 1, 1});
            continue;
        }

        const TexSource *src = source_for(p, rams);
        if (!src) continue;
        const int prow = palette_row(p, rams);
        if (prow < 0) continue;
        const float pal_coord = (float(prow) + 0.5f) / float(kPaletteRows);
        const Prog prog = p.checker ? Prog::ModelChecker
                                    : (src->transparent ? Prog::ModelAlpha : Prog::Model);
        const uint32_t bi = batch_for(clip, prog, src->texture, depth);
        g.batches[bi].poly_refs.push_back({&p, depth, 0xffffffffu, pal_coord, src->w, src->h});
    }

    flush_palette_gl();
    last_batch_count_ = g.batch_count;

    for (size_t bi = 0; bi < g.batch_count; ++bi) {
        const Batch &b = g.batches[bi];
        if (b.poly_refs.empty()) continue;
        g.clip = b.clip;

        size_t quads = 0;
        for (const PolyRef &ref : b.poly_refs) quads += size_t(ref.poly->n - 1) / 2;
        Vertex *quad_dst = append_quads(quads);
        const bool indexed = quad_dst != nullptr;
        const uint32_t first = indexed ? uint32_t(quad_dst - g.verts.data()) : uint32_t(g.verts.size);
        uint32_t written = 0;

        for (const PolyRef &ref : b.poly_refs) {
            const gpu3d_poly &poly = *ref.poly;
            const int n = poly.n;
            const bool tex = (poly.renderer & 2) != 0;
            const float inv_tw = tex ? (1.0f / float(ref.tex_w)) : 0.0f;
            const float inv_th = tex ? (1.0f / float(ref.tex_h)) : 0.0f;
            Vertex v[8];
            bool valid = true;
            for (int k = 0; k < n && valid; ++k) {
                const float x_disp = ox + poly.x[k] * sx;
                const float y_disp = oy + poly.y[k] * sy;
                float w = 1.0f;
                float u = 0.0f, vv = 0.0f;
                if (tex) {
                    const float ooz = poly.z[k];
                    if (!std::isfinite(ooz) || ooz <= 1e-9f) { valid = false; break; }
                    w = 1.0f / ooz;
                    u = poly.u[k] * w * inv_tw;
                    vv = poly.v[k] * w * inv_th;
                }
                v[k] = {x_disp * w, y_disp * w, w, u, vv, ref.palette, ref.depth, ref.color};
                valid = std::isfinite(v[k].x) && std::isfinite(v[k].y) && std::isfinite(w) &&
                        std::isfinite(u) && std::isfinite(vv);
            }
            if (!valid) continue;
            if (indexed) {
                written += uint32_t(write_fan_quads(quad_dst + written, v, n));
            } else {
                const size_t tcnt = size_t(n - 2) * 3;
                Vertex *dst = g.verts.append(tcnt);
                for (int k = 1; k + 1 < n; ++k) {
                    *dst++ = v[0];
                    *dst++ = v[k];
                    *dst++ = v[k + 1];
                }
                written += uint32_t(tcnt);
            }
        }
        if (indexed) g.verts.size = first + written;
        if (written) g.cmds.push_back({b.prog, b.texture, first, written, Depth::TestWrite, g.clip, false, indexed});
    }
    g.clip = -1;
}

void GpuGlesRenderer::draw_frame(const model2a::picture_parts &parts, int w2d, AspectMode aspect,
                                 const gpu3d_poly *polys, size_t poly_count, const gpu3d_rams *rams) {
    if (!ok_) return;
    const int w = parts.w > 0 ? parts.w : w2d;
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    compute_viewport(w, aspect, sx, sy, ox, oy);

    const int pad = parts.bd_unpadded ? 0 : model2a::picture_parts::PAD;
    if (parts.version != last_2d_version_) {
        last_2d_version_ = parts.version;
        if (parts.backdrop) {
            update_texture_2d_rgb565(layer_bd_, w + 2 * pad, 384 + 2 * pad, parts.filter,
                                     parts.backdrop, size_t(w + 2 * pad));
        }
        draw_mid_y0_ = std::clamp(parts.mid_y0, 0, 384);
        draw_mid_y1_ = std::clamp(parts.mid_y1, 0, 384);
        mid_empty_ = !parts.mid || draw_mid_y1_ <= draw_mid_y0_;
        if (!mid_empty_) {
            update_texture_2d_rgba5551(layer_mid_, w, 384, false, parts.mid, size_t(w),
                                       draw_mid_y0_, draw_mid_y1_, prev_mid_y0_, prev_mid_y1_);
        }
        draw_top_y0_ = std::clamp(parts.top_y0, 0, 384);
        draw_top_y1_ = std::clamp(parts.top_y1, 0, 384);
        top_empty_ = !parts.top || draw_top_y1_ <= draw_top_y0_;
        if (!top_empty_) {
            update_texture_2d_rgba5551(layer_top_, w, 384, false, parts.top, size_t(w),
                                       draw_top_y0_, draw_top_y1_, prev_top_y0_, prev_top_y1_);
        }
    }

    const int vx0 = std::clamp(int(std::floor(ox)), 0, g.display_w);
    const int vy0 = std::clamp(int(std::floor(oy)), 0, g.display_h);
    const int vx1 = std::clamp(int(std::ceil(ox + float(w) * sx)), 0, g.display_w);
    const int vy1 = std::clamp(int(std::ceil(oy + 384.0f * sy)), 0, g.display_h);
    const int screen_clip = add_clip({vx0, vy0, vx1, vy1});

    // 1. Backdrop
    if (const uint32_t tex_bd = layer_bd_.active()) {
        const float s = float(std::max(parts.s, 1));
        const float bx0 = ox + (-float(pad) - float(parts.bdx) / s) * sx;
        const float by0 = oy + (-float(pad) - float(parts.bdy) / s) * sy;
        const float bx1 = bx0 + float(w + 2 * pad) * sx;
        const float by1 = by0 + float(384 + 2 * pad) * sy;
        g.clip = screen_clip;
        push_quad(Prog::Ui, tex_bd, bx0, by0, bx1, by1, 0, 0, 1, 1, 0xffffffffu, Depth::Off, false);
        g.clip = -1;
    }

    // 2. Mid 2D (only active row band [draw_mid_y0_, draw_mid_y1_))
    if (!mid_empty_ && layer_mid_.active() && draw_mid_y1_ > draw_mid_y0_) {
        const float v0 = float(draw_mid_y0_) * (1.0f / 384.0f);
        const float v1 = float(draw_mid_y1_) * (1.0f / 384.0f);
        g.clip = screen_clip;
        push_quad(Prog::Ui, layer_mid_.active(),
                  ox, oy + float(draw_mid_y0_) * sy,
                  ox + float(w) * sx, oy + float(draw_mid_y1_) * sy,
                  0.0f, v0, 1.0f, v1, 0xffffffffu, Depth::Off, true);
        g.clip = -1;
    }

    // 3. 3D Scene (Mali-G31 GLES2 or CPU fallback map3d)
    if (parts.gpu3d && rams) {
        emit_3d_polys(polys, poly_count, *rams, w, sx, sy, ox, oy);
    } else if (parts.has3d && parts.map3d) {
        const int s = std::max(parts.s, 1);
        update_texture_rgba(tex_cpu3d_, cpu3d_w_, cpu3d_h_, w * s, 384 * s, false,
                            parts.map3d, parts.map_stride);
        if (tex_cpu3d_) {
            g.clip = screen_clip;
            push_quad(Prog::Ui, tex_cpu3d_, ox, oy, ox + float(w) * sx, oy + 384.0f * sy,
                      0, 0, 1, 1, 0xffffffffu, Depth::Off, true);
            g.clip = -1;
        }
        last_poly_count_ = 0;
        last_batch_count_ = 0;
    }

    // 4. Top 2D (HUD): emit non-empty sub-bands so Mali-G31 skips blending transparent center rows
    if (!top_empty_ && layer_top_.active() && draw_top_y1_ > draw_top_y0_) {
        g.clip = screen_clip;
        const std::vector<uint16_t> &sh = layer_top_.shadow[layer_top_.cur];
        // Find if there is a large empty gap (>= 64 rows) in the middle of the screen (e.g. between top lifebars and bottom credits)
        int gap_y0 = -1, gap_y1 = -1;
        if (draw_top_y1_ - draw_top_y0_ > 96 && sh.size() == size_t(w) * 384u) {
            int cur_g0 = -1;
            int best_len = 0;
            for (int y = draw_top_y0_; y < draw_top_y1_; ++y) {
                const uint64_t *row64 = reinterpret_cast<const uint64_t *>(sh.data() + size_t(y) * size_t(w));
                uint64_t acc = 0;
                const int n64 = w / 4;
                for (int i = 0; i < n64; ++i) { acc |= row64[i]; if (acc) break; }
                if (acc == 0) {
                    if (cur_g0 < 0) cur_g0 = y;
                } else if (cur_g0 >= 0) {
                    if (y - cur_g0 > best_len) { best_len = y - cur_g0; gap_y0 = cur_g0; gap_y1 = y; }
                    cur_g0 = -1;
                }
            }
            if (cur_g0 >= 0 && draw_top_y1_ - cur_g0 > best_len) {
                best_len = draw_top_y1_ - cur_g0;
                gap_y0 = cur_g0;
                gap_y1 = draw_top_y1_;
            }
            if (best_len < 64) gap_y0 = gap_y1 = -1;
        }
        auto emit_top_band = [&](int y0, int y1) {
            if (y1 <= y0) return;
            const float v0 = float(y0) * (1.0f / 384.0f);
            const float v1 = float(y1) * (1.0f / 384.0f);
            push_quad(Prog::Ui, layer_top_.active(),
                      ox, oy + float(y0) * sy,
                      ox + float(w) * sx, oy + float(y1) * sy,
                      0.0f, v0, 1.0f, v1, 0xffffffffu, Depth::Off, true);
        };
        if (gap_y0 >= 0) {
            emit_top_band(draw_top_y0_, gap_y0);
            emit_top_band(gap_y1, draw_top_y1_);
        } else {
            emit_top_band(draw_top_y0_, draw_top_y1_);
        }
        g.clip = -1;
    }

    // 5. 4:3 screen sides in widescreen (character select painted sides or solid borders)
    const int side_w = (w - model2a::WIDTH) / 2;
    if (side_w > 0 && parts.sides == 1) {
        const float lw = float(side_w) * sx;
        const float rx = ox + float(w - side_w) * sx;
        push_quad(Prog::Flat, 0, ox, oy, ox + lw, oy + 384.0f * sy, 0, 0, 0, 0,
                  argb_to_rgba(parts.side_colour[0] | 0xff000000u), Depth::Off, false);
        push_quad(Prog::Flat, 0, rx, oy, rx + lw, oy + 384.0f * sy, 0, 0, 0, 0,
                  argb_to_rgba(parts.side_colour[1] | 0xff000000u), Depth::Off, false);
    } else if (side_w > 0 && parts.sides == 2 && parts.side_px) {
        const int s = std::max(parts.s, 1);
        const int sw_px = side_w * s;
        if (parts.version != last_sides_version_ || !tex_sides_ || sides_w_ != 2 * sw_px || sides_h_ != 384 * s) {
            last_sides_version_ = parts.version;
            update_texture_rgba(tex_sides_, sides_w_, sides_h_, 2 * sw_px, 384 * s, false,
                                parts.side_px, size_t(2 * sw_px));
        }
        if (tex_sides_) {
            const float lw = float(side_w) * sx;
            const float rx = ox + float(w - side_w) * sx;
            push_quad(Prog::Ui, tex_sides_, ox, oy, ox + lw, oy + 384.0f * sy,
                      0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu, Depth::Off, false);
            push_quad(Prog::Ui, tex_sides_, rx, oy, rx + lw, oy + 384.0f * sy,
                      0.5f, 0.0f, 1.0f, 1.0f, 0xffffffffu, Depth::Off, false);
        }
    }
}

void GpuGlesRenderer::draw_cpu_framebuffer(const uint32_t *argb, int w, int h, AspectMode aspect) {
    if (!ok_ || !argb || w <= 0 || h <= 0) return;
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    compute_viewport(w, aspect, sx, sy, ox, oy);
    update_texture_rgba(tex_cpu3d_, cpu3d_w_, cpu3d_h_, w, h, false, argb, size_t(w));
    if (tex_cpu3d_) {
        push_quad(Prog::Ui, tex_cpu3d_, ox, oy, ox + float(w) * sx, oy + float(h) * sy,
                  0, 0, 1, 1, 0xffffffffu, Depth::Off, false);
    }
}

inline uint64_t hash_tex_rect(uint32_t base_x, uint32_t base_y, uint32_t w, uint32_t h, const uint32_t *sheet) {
    uint64_t hval = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y += 2) {
        const uint32_t ry = base_y + y;
        for (uint32_t x = 0; x < w; x += 4) {
            uint32_t x2 = base_x + x;
            uint32_t y2 = ry;
            const uint32_t right = x2 >= 1024 ? 1024u : 0u;
            x2 -= right;
            y2 ^= right;
            const uint32_t offset = ((y2 >> 1) << 9) + (x2 >> 1);
            hval ^= sheet[offset >> 1];
            hval *= 0x100000001b3ull;
        }
    }
    return hval;
}

} // namespace arm64
