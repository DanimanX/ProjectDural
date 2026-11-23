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
void gl_begin_frame(bool scene = true);
void gl_resolve_scene();
void gl_end_frame();
void gl_fini();
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba);
float gl_render_scale();
void gl_set_render_scale(float scale);
// The driver's glFinish, for benchmarks that must see the work land. Null if unavailable.
void (*gl_finish_ptr())(void);
// Split of the last frame's GL time: resident texture updates vs draw submission.
double gl_upload_ms();
double gl_submit_ms();

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
    uint32_t last_tex_clears() const { return last_tex_clears_; }
    // Per-polygon scissor (see poly_scissor_): kept for A/B runs and the autobench.
    void set_poly_scissor(bool on) { poly_scissor_ = on; }
    bool poly_scissor() const { return poly_scissor_; }
    uint32_t last_pal_clears() const { return last_pal_clears_; }

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
        bool stale = false;
        size_t bytes = 0;
        uint64_t hash = 0;
    };

    static constexpr size_t kSourceCacheBytes = 24u * 1024u * 1024u;
    static constexpr uint32_t kTextureLimit = 512;
    static constexpr uint32_t kPaletteWidth = 128;
    static constexpr uint32_t kPaletteRows = 2048;

    struct LayerTex2D {
        uint32_t tex[2] = {0, 0};
        uint32_t cur = 0;
        int w = 0, h = 0;
        bool linear = false;
        std::vector<uint16_t> shadow[2];
        uint32_t active() const { return tex[cur]; }
    };

    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                          uint32_t format, const void *pixels);
    void update_texture_rgba(uint32_t &tex, int &cur_w, int &cur_h, int w, int h, bool linear,
                             const uint32_t *argb, size_t stride_px);
    void update_texture_2d_rgb565(LayerTex2D &layer, int w, int h, bool linear,
                                  const uint32_t *argb, size_t stride_px);
    void update_texture_2d_rgba5551(LayerTex2D &layer, int w, int h, bool linear,
                                    const uint32_t *argb, size_t stride_px,
                                    int y_active0, int y_active1, int &prev_y0, int &prev_y1);
    const TexSource *source_for(const gpu3d_poly &poly, const gpu3d_rams &rams);
    int palette_row(const gpu3d_poly &poly, const gpu3d_rams &rams);
    void clear_tex_cache();
    void clear_tex_sheet(uint32_t sheet);
    void clear_pal_cache();
    void flush_palette_gl();
    void emit_3d_polys(const gpu3d_poly *polys, size_t count, const gpu3d_rams &rams,
                       int w2d, float sx, float sy, float ox, float oy);

    bool ok_ = false;
    bool shutdown_ = false;

    // 2D Layer textures (double-buffered to avoid Mali-G31 texture modification pipeline stalls)
    LayerTex2D layer_bd_, layer_mid_, layer_top_;
    uint32_t tex_cpu3d_ = 0, tex_sides_ = 0;
    int cpu3d_w_ = 0, cpu3d_h_ = 0;
    int sides_w_ = 0, sides_h_ = 0;
    uint64_t last_2d_version_ = UINT64_MAX;
    uint64_t last_sides_version_ = UINT64_MAX;
    int prev_mid_y0_ = 0, prev_mid_y1_ = 384;
    int prev_top_y0_ = 0, prev_top_y1_ = 384;
    int draw_mid_y0_ = 0, draw_mid_y1_ = 0;
    int draw_top_y0_ = 0, draw_top_y1_ = 0;
    bool mid_empty_ = true, top_empty_ = true;

    // 3D Texture cache
    std::unordered_map<TexKey, TexSource, TexKeyHash> sources_;
    std::vector<uint8_t> index_scratch_;
    size_t cached_bytes_ = 0;
    uint32_t last_tex_gen_[2] = {UINT32_MAX, UINT32_MAX};

    // 3D Palette LUT cache (128 x 2048 RGB565)
    uint32_t palette_texture_ = 0;
    std::vector<uint16_t> palette_shadow_;
    std::unordered_map<uint64_t, uint32_t> palette_index_;
    uint32_t palette_used_ = 0;
    uint32_t palette_dirty_begin_ = UINT32_MAX;
    uint32_t palette_dirty_end_ = 0;
    uint32_t last_luma_gen_ = UINT32_MAX;
    uint32_t last_xlat_gen_ = UINT32_MAX;
    uint8_t last_gamma_[256] = {};
    bool have_gamma_ = false;

    std::vector<uint32_t> swizzle_scratch_;
    // Per-polygon bounding scissor. On by default (what the renderer was tuned for on
    // Mali-G31); turned off for tile-based GPUs that pay for each scissor change, where
    // one scissor for the whole 3D pass also lets batching merge far more polygons.
    bool poly_scissor_ = true;
    size_t last_poly_count_ = 0;
    size_t last_batch_count_ = 0;
    uint32_t last_tex_clears_ = 0;
    uint32_t last_pal_clears_ = 0;
};

} // namespace arm64
