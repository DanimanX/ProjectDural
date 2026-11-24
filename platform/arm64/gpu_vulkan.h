// Native SDL3 SDL_GPU (Vulkan + SPIR-V) renderer for ProjectDural
// (ARM64 Linux / Mesa Turnip / Adreno / Mali / Desktop Vulkan).
//
// Exposes the same arm64::GlConfig / gl_* / GpuGlesRenderer / ImGuiGles
// surface as gpu_gles.h so main_arm64.cpp drives both backends identically,
// while delegating 3D polygon rasterization to src/gpu/gpu3d_sdlgpu.cpp
// (pixel-exact Model 2A compute shaders in SPIR-V).
#pragma once

#include "gpu/gpu3d.h"
#include "machine/model2a.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arm64 {

enum class AspectMode : int {
    Widescreen32 = 0,
    Original43 = 1,
    StretchFull = 2
};

struct GlConfig {
    bool fullscreen = true;
    bool vsync = true;
    int window_w = 1280;
    int window_h = 720;
};

bool gl_init(const GlConfig &cfg);
const char *gl_error();
const char *gl_renderer_string();
inline bool gl_is_gles() { return false; }
SDL_Window *gl_window();
SDL_GPUDevice *vk_device();
int gl_display_width();
int gl_display_height();
void gl_update_drawable_size();
void gl_set_vsync(bool enabled);
void gl_begin_frame(bool scene = true);
void gl_resolve_scene();
void gl_end_frame();
void gl_fini();
float gl_render_scale();
void gl_set_render_scale(float scale);
void (*gl_finish_ptr())(void);
double gl_upload_ms();
double gl_submit_ms();
void gl_process_event(const SDL_Event &e);

class GpuVulkanRenderer {
public:
    GpuVulkanRenderer();
    ~GpuVulkanRenderer();
    GpuVulkanRenderer(const GpuVulkanRenderer &) = delete;
    GpuVulkanRenderer &operator=(const GpuVulkanRenderer &) = delete;

    bool ok() const { return ok_; }
    void shutdown();
    void reset_caches();

    void draw_frame(const model2a::picture_parts &parts, int w2d, AspectMode aspect,
                    const gpu3d_poly *polys, size_t poly_count, const gpu3d_rams *rams);
    void draw_cpu_framebuffer(const uint32_t *argb, int w, int h, AspectMode aspect);

    // Diagnostics
    size_t last_poly_count() const { return last_poly_count_; }
    size_t last_batch_count() const { return last_batch_count_; }
    size_t cached_tex_bytes() const { return 0; }
    size_t cached_tex_count() const { return 0; }
    uint32_t last_tex_clears() const { return 0; }
    void set_poly_scissor(bool on) { poly_scissor_ = on; }
    bool poly_scissor() const { return poly_scissor_; }
    uint32_t last_pal_clears() const { return 0; }

private:
    struct LayerTex {
        SDL_GPUTexture *tex = nullptr;
        int w = 0;
        int h = 0;
    };

    void ensure_layer(LayerTex &lt, int w, int h);
    void upload_layer(LayerTex &lt, int w, int h, const uint32_t *pixels, size_t stride_px);
    void release_layer(LayerTex &lt);

    bool ok_ = false;
    bool shutdown_ = false;
    bool poly_scissor_ = true;

    std::unique_ptr<gpu3d> gpu3d_;
    int target_idx_ = 0;
    int target_w_ = 0;
    int target_h_ = 0;

    LayerTex layer_bd_, layer_mid_, layer_top_, layer_cpu_, layer_sides_;
    uint64_t last_2d_version_ = UINT64_MAX;
    uint64_t last_sides_version_ = UINT64_MAX;
    std::vector<uint32_t> bd_scratch_;

    size_t last_poly_count_ = 0;
    size_t last_batch_count_ = 0;
};

using GpuGlesRenderer = GpuVulkanRenderer;

} // namespace arm64
