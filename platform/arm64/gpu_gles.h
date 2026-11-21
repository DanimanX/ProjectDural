// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 renderer for ProjectDural
// (Anbernic RG34XX Mali-G31 MP2 and compatible AArch64 Linux handhelds).
//
// * Reproduces Model 2A front-to-back polygon priority via strictly decreasing
//   depth ranks with GL_GREATER + depth clear 0.0, allowing aggressive draw-call
//   batching by (scissor_vp, program, texture) on Mali-G31.
// * Perspective-correct textured & solid polygons, checkerboard translucency,
//   4-bit swizzled texel cache (GL_LUMINANCE_ALPHA) + 128x2048 RGBA palette LUT.
// * ARM64 NEON 0xAARRGGBB -> GL_RGBA swizzle for System 24 2D layers and
//   optional CPU 3D fallback (4:3 screens in 3:2 widescreen & render test mode).
#pragma once

#include "gpu/gpu3d.h"
#include "machine/model2a.h"

#include <SDL.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#ifndef RGBA8
#define RGBA8(r, g, b, a) \
    ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(b) & 0xFFu) << 16) | \
     (((uint32_t)(g) & 0xFFu) << 8) | (((uint32_t)(r) & 0xFFu) << 0))
#endif

namespace arm64 {

enum class AspectMode : int {
    Widescreen32 = 0, // Native 3:2 widened Model 2A frustum (576x384 -> 720x480 on RG34XX)
    Original43 = 1,   // Authentic 4:3 (496x384 -> 620x480 centered)
    StretchFull = 2   // 496x384 stretched to full display
};

struct GlConfig {
    bool fullscreen = true;
    bool vsync = true;
    int window_w = 720;
    int window_h = 480;
};

bool gl_init(const GlConfig &cfg);
const char *gl_error();
const char *gl_renderer_string();
bool gl_is_gles();
SDL_Window *gl_window();
int gl_display_width();
int gl_display_height();
void gl_update_drawable_size();
void gl_set_vsync(bool enabled);
void gl_begin_frame();
void gl_end_frame();
void gl_fini();
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba);

struct GlUiVertex { float x, y, u, v; uint32_t rgba; };
uint32_t gl_create_ui_texture(int w, int h, const void *rgba);
void gl_delete_ui_texture(uint32_t texture);
void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1);

class GpuGlesRenderer {
public:
    GpuGlesRenderer();
    ~GpuGlesRenderer();
    GpuGlesRenderer(const GpuGlesRenderer &) = delete;
    GpuGlesRenderer &operator=(const GpuGlesRenderer &) = delete;

    bool ok() const { return ok_; }
    void shutdown();
    void reset_caches();

    void draw_frame(const model2a::picture_parts &parts, int w2d, AspectMode aspect,
                    const gpu3d_poly *polys, size_t poly_count, const gpu3d_rams *rams);
    void draw_cpu_framebuffer(const uint32_t *argb, int w, int h, AspectMode aspect);

    // Diagnostics
    size_t last_poly_count() const { return last_poly_count_; }
    size_t last_batch_count() const { return last_batch_count_; }
    size_t cached_tex_bytes() const { return cached_bytes_; }
    size_t cached_tex_count() const { return sources_.size(); }

private:
    struct TexKey {
        uint64_t val = 0;
        bool operator==(const TexKey &o) const { return val == o.val; }
    };
    struct TexKeyHash {
        size_t operator()(const TexKey &k) const {
            uint64_t x = k.val;
            x ^= x >> 33; x *= 0xff51afd7ed558ccduLL;
            x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53uLL;
            return size_t(x ^ (x >> 33));
        }
    };
    struct TexSource {
        uint32_t texture = 0;
        uint32_t w = 1, h = 1;
        bool transparent = false;
        size_t bytes = 0;
    };

    static constexpr size_t kSourceCacheBytes = 24u * 1024u * 1024u;
    static constexpr uint32_t kTextureLimit = 512;
    static constexpr uint32_t kPaletteWidth = 128;
    static constexpr uint32_t kPaletteRows = 2048;

    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                          uint32_t format, const void *pixels);
    void update_texture_rgba(uint32_t &tex, int &cur_w, int &cur_h, int w, int h, bool linear,
                             const uint32_t *argb, size_t stride_px);
    const TexSource *source_for(const gpu3d_poly &poly, const gpu3d_rams &rams);
    int palette_row(const gpu3d_poly &poly, const gpu3d_rams &rams);
    void clear_tex_cache();
    void clear_pal_cache();
    void flush_palette_gl();
    void emit_3d_polys(const gpu3d_poly *polys, size_t count, const gpu3d_rams &rams,
                       int w2d, float sx, float sy, float ox, float oy);

    bool ok_ = false;
    bool shutdown_ = false;

    // 2D Layer textures
    uint32_t tex_bd_ = 0, tex_mid_ = 0, tex_top_ = 0, tex_cpu3d_ = 0, tex_sides_ = 0;
    int bd_w_ = 0, bd_h_ = 0;
    int mid_w_ = 0, mid_h_ = 0;
    int top_w_ = 0, top_h_ = 0;
    int cpu3d_w_ = 0, cpu3d_h_ = 0;
    int sides_w_ = 0, sides_h_ = 0;
    uint64_t last_2d_version_ = UINT64_MAX;
    bool mid_empty_ = true, top_empty_ = true;

    // 3D Texture cache
    std::unordered_map<TexKey, TexSource, TexKeyHash> sources_;
    std::vector<uint8_t> index_scratch_;
    size_t cached_bytes_ = 0;
    uint32_t last_tex_gen_[2] = {UINT32_MAX, UINT32_MAX};

    // 3D Palette LUT cache (128 x 2048 RGBA)
    uint32_t palette_texture_ = 0;
    std::vector<uint32_t> palette_shadow_;
    std::unordered_map<uint64_t, uint32_t> palette_index_;
    uint32_t palette_used_ = 0;
    uint32_t palette_dirty_begin_ = UINT32_MAX;
    uint32_t palette_dirty_end_ = 0;
    uint32_t last_luma_gen_ = UINT32_MAX;
    uint32_t last_xlat_gen_ = UINT32_MAX;
    uint8_t last_gamma_[256] = {};
    bool have_gamma_ = false;

    std::vector<uint32_t> swizzle_scratch_;
    size_t last_poly_count_ = 0;
    size_t last_batch_count_ = 0;
};

} // namespace arm64
