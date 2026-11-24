// Native SDL3 SDL_GPU (Vulkan + SPIR-V) renderer implementation for ProjectDural.
#include "gpu_vulkan.h"
#include "gpu_vulkan_shaders.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

std::unique_ptr<gpu3d> gpu3d_create_sdlgpu(SDL_GPUDevice *dev);

namespace arm64 {

namespace {

enum class QuadBlend : uint8_t { Opaque, Alpha, Premul };

struct QuadDraw {
    SDL_GPUTexture *tex = nullptr;
    bool linear = false;
    QuadBlend blend = QuadBlend::Opaque;
    float dst_x = -1.0f, dst_y = -1.0f, dst_w = 2.0f, dst_h = 2.0f;
    float src_u = 0.0f, src_v = 0.0f, src_uw = 1.0f, src_vh = 1.0f;
    bool force_opaque = false;
};

struct PendingUpload {
    SDL_GPUTexture *tex = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t offset = 0;
};

struct VkState {
    SDL_Window *window = nullptr;
    SDL_GPUDevice *dev = nullptr;
    SDL_GPUShader *vert_shader = nullptr;
    SDL_GPUShader *frag_shader = nullptr;
    SDL_GPUGraphicsPipeline *pipe_opaque = nullptr;
    SDL_GPUGraphicsPipeline *pipe_alpha = nullptr;
    SDL_GPUGraphicsPipeline *pipe_premul = nullptr;
    SDL_GPUSampler *samp_nearest = nullptr;
    SDL_GPUSampler *samp_linear = nullptr;
    SDL_GPUTransferBuffer *upload_buf = nullptr;
    uint32_t upload_cap = 0;
    uint32_t upload_used = 0;
    uint8_t *upload_mapped = nullptr;

    int disp_w = 1280;
    int disp_h = 720;
    bool vsync = true;
    float render_scale = 1.0f;
    bool imgui_active = false;

    std::string error;
    std::string renderer_str = "Vulkan (SDL_GPU)";

    std::vector<QuadDraw> quads;
    std::vector<PendingUpload> uploads;
    double upload_ms = 0.0;
    double submit_ms = 0.0;
} g;

using clk = std::chrono::steady_clock;
inline double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

void compute_viewport(AspectMode aspect, int w2d, float &vx, float &vy, float &vw, float &vh) {
    const float dw = float(std::max(1, g.disp_w));
    const float dh = float(std::max(1, g.disp_h));
    if (aspect == AspectMode::StretchFull) {
        vx = 0.0f; vy = 0.0f; vw = dw; vh = dh;
        return;
    }
    const float target_ar = (aspect == AspectMode::Widescreen32)
        ? (float(w2d) / float(model2a::HEIGHT))
        : (496.0f / 384.0f);
    const float disp_ar = dw / dh;
    if (disp_ar > target_ar) {
        vh = dh;
        vw = dh * target_ar;
        vx = (dw - vw) * 0.5f;
        vy = 0.0f;
    } else {
        vw = dw;
        vh = dw / target_ar;
        vx = 0.0f;
        vy = (dh - vh) * 0.5f;
    }
}

// A rectangle in display pixels (top-left origin, +Y down) to NDC for the quad shader, which lays
// its corners out as dst + corner * dst_size with texture v growing with +Y. SDL_GPU's NDC has +Y
// up (SDL_gpu.h, Coordinate System), so the top edge (py) is the quad's *upper* NDC edge: without
// this, every layer lands upside down (and the game with it).
void px_to_ndc(float px, float py, float pw, float ph, float &nx, float &ny, float &nw, float &nh) {
    const float dw = float(std::max(1, g.disp_w));
    const float dh = float(std::max(1, g.disp_h));
    nx = (px / dw) * 2.0f - 1.0f;
    ny = 1.0f - ((py + ph) / dh) * 2.0f;   // the bottom edge; +nh reaches the top edge (py)
    nw = (pw / dw) * 2.0f;
    nh = (ph / dh) * 2.0f;
}

bool ensure_upload_capacity(uint32_t needed_bytes) {
    if (g.upload_buf && g.upload_cap >= needed_bytes) return true;
    if (g.upload_mapped) {
        SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_mapped = nullptr;
    }
    if (g.upload_buf) {
        SDL_ReleaseGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_buf = nullptr;
        g.upload_cap = 0;
    }
    const uint32_t cap = std::max(needed_bytes, 8u * 1024u * 1024u);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = cap;
    g.upload_buf = SDL_CreateGPUTransferBuffer(g.dev, &tbi);
    if (!g.upload_buf) return false;
    g.upload_cap = cap;
    return true;
}

void queue_texture_upload(SDL_GPUTexture *tex, int w, int h, const uint32_t *pixels, size_t stride_px) {
    if (!tex || !pixels || w <= 0 || h <= 0) return;
    const uint32_t row_bytes = uint32_t(w) * 4u;
    const uint32_t total_bytes = row_bytes * uint32_t(h);
    const uint32_t aligned_off = (g.upload_used + 15u) & ~15u;
    if (!ensure_upload_capacity(aligned_off + total_bytes)) return;
    if (!g.upload_mapped) {
        g.upload_mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(g.dev, g.upload_buf, true));
        if (!g.upload_mapped) return;
    }
    uint8_t *dst = g.upload_mapped + aligned_off;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + size_t(y) * row_bytes, pixels + size_t(y) * stride_px, row_bytes);
    }
    g.uploads.push_back({tex, uint32_t(w), uint32_t(h), aligned_off});
    g.upload_used = aligned_off + total_bytes;
}

SDL_GPUGraphicsPipeline *make_quad_pipeline(SDL_GPUTextureFormat fmt, QuadBlend blend) {
    SDL_GPUColorTargetBlendState bs{};
    if (blend == QuadBlend::Alpha) {
        bs.enable_blend = true;
        bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
        bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    } else if (blend == QuadBlend::Premul) {
        bs.enable_blend = true;
        bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
        bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = fmt;
    ctd.blend_state = bs;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = g.vert_shader;
    pci.fragment_shader = g.frag_shader;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.target_info.num_color_targets = 1;
    pci.target_info.color_target_descriptions = &ctd;
    return SDL_CreateGPUGraphicsPipeline(g.dev, &pci);
}

void vk_finish_impl() {
    if (g.dev) SDL_WaitForGPUIdle(g.dev);
}

void apply_present_mode() {
    if (!g.dev || !g.window) return;
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!g.vsync) {
        if (SDL_WindowSupportsGPUPresentMode(g.dev, g.window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        } else if (SDL_WindowSupportsGPUPresentMode(g.dev, g.window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
    }
    SDL_SetGPUSwapchainParameters(g.dev, g.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

} // namespace

void vk_set_imgui_active(bool active) { g.imgui_active = active; }

bool gl_init(const GlConfig &cfg) {
    g.error.clear();
    g.vsync = cfg.vsync;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            g.error = std::string("SDL_InitSubSystem(VIDEO) failed: ") + SDL_GetError();
            return false;
        }
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    g.window = SDL_CreateWindow("ProjectDural — Virtua Fighter 2 (Vulkan)",
                                cfg.window_w > 0 ? cfg.window_w : 1280,
                                cfg.window_h > 0 ? cfg.window_h : 720, flags);
    if (!g.window) {
        g.error = std::string("SDL_CreateWindow(Vulkan) failed: ") + SDL_GetError();
        return false;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, std::getenv("GPU3D_VALIDATE") != nullptr);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    g.dev = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g.dev) {
        g.error = std::string("SDL_CreateGPUDevice(Vulkan) failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(g.dev, g.window)) {
        g.error = std::string("SDL_ClaimWindowForGPUDevice failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    apply_present_mode();
    gl_update_drawable_size();

    if (const char *drv = SDL_GetGPUDeviceDriver(g.dev)) {
        g.renderer_str = std::string("SDL_GPU ") + drv;
    }

    SDL_GPUShaderCreateInfo vsi{};
    vsi.code = kQuadVertSpv;
    vsi.code_size = sizeof(kQuadVertSpv);
    vsi.entrypoint = "main";
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vsi.num_uniform_buffers = 1;
    g.vert_shader = SDL_CreateGPUShader(g.dev, &vsi);

    SDL_GPUShaderCreateInfo fsi{};
    fsi.code = kQuadFragSpv;
    fsi.code_size = sizeof(kQuadFragSpv);
    fsi.entrypoint = "main";
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.num_samplers = 1;
    fsi.num_uniform_buffers = 1;
    g.frag_shader = SDL_CreateGPUShader(g.dev, &fsi);

    const SDL_GPUTextureFormat sc_fmt = SDL_GetGPUSwapchainTextureFormat(g.dev, g.window);
    if (g.vert_shader && g.frag_shader) {
        g.pipe_opaque = make_quad_pipeline(sc_fmt, QuadBlend::Opaque);
        g.pipe_alpha = make_quad_pipeline(sc_fmt, QuadBlend::Alpha);
        g.pipe_premul = make_quad_pipeline(sc_fmt, QuadBlend::Premul);
    }

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_NEAREST;
    sci.mag_filter = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g.samp_nearest = SDL_CreateGPUSampler(g.dev, &sci);

    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    g.samp_linear = SDL_CreateGPUSampler(g.dev, &sci);

    if (!g.pipe_opaque || !g.pipe_alpha || !g.pipe_premul || !g.samp_nearest || !g.samp_linear) {
        g.error = std::string("SDL_GPU pipeline/sampler init failed: ") + SDL_GetError();
        gl_fini();
        return false;
    }

    return true;
}

const char *gl_error() { return g.error.c_str(); }
const char *gl_renderer_string() { return g.renderer_str.c_str(); }
SDL_Window *gl_window() { return g.window; }
SDL_GPUDevice *vk_device() { return g.dev; }
int gl_display_width() { return g.disp_w; }
int gl_display_height() { return g.disp_h; }

void gl_update_drawable_size() {
    if (!g.window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g.window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(g.window, &w, &h);
    g.disp_w = std::max(1, w);
    g.disp_h = std::max(1, h);
}

void gl_set_vsync(bool enabled) {
    g.vsync = enabled;
    apply_present_mode();
}

float gl_render_scale() { return g.render_scale; }
void gl_set_render_scale(float scale) { g.render_scale = std::clamp(scale, 0.5f, 6.0f); }
void (*gl_finish_ptr())(void) { return vk_finish_impl; }
double gl_upload_ms() { return g.upload_ms; }
double gl_submit_ms() { return g.submit_ms; }

void gl_process_event(const SDL_Event &e) {
    if (g.imgui_active) ImGui_ImplSDL3_ProcessEvent(&e);
}

void gl_begin_frame(bool /*scene*/) {
    g.quads.clear();
    g.uploads.clear();
    g.upload_used = 0;
}

void gl_resolve_scene() {}

void gl_end_frame() {
    if (!g.dev || !g.window) return;
    const auto t0 = clk::now();

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g.dev);
    if (!cb) return;

    if (g.upload_mapped) {
        SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
        g.upload_mapped = nullptr;
    }
    if (!g.uploads.empty() && g.upload_buf) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        for (const auto &u : g.uploads) {
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = g.upload_buf;
            src.offset = u.offset;
            src.pixels_per_row = u.w;
            src.rows_per_layer = u.h;
            SDL_GPUTextureRegion dst{};
            dst.texture = u.tex;
            dst.w = u.w;
            dst.h = u.h;
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(cp);
    }
    g.upload_ms = ms_since(t0);

    ImDrawData *draw_data = g.imgui_active ? ImGui::GetDrawData() : nullptr;
    if (draw_data) {
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cb);
    }

    SDL_GPUTexture *swap_tex = nullptr;
    Uint32 sw = 0, sh = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cb, g.window, &swap_tex, &sw, &sh) && swap_tex) {
        if (sw > 0 && sh > 0) {
            g.disp_w = int(sw);
            g.disp_h = int(sh);
        }
        SDL_GPUColorTargetInfo cti{};
        cti.texture = swap_tex;
        cti.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        cti.load_op = SDL_GPU_LOADOP_CLEAR;
        cti.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &cti, 1, nullptr);
        for (const auto &q : g.quads) {
            if (!q.tex) continue;
            SDL_GPUGraphicsPipeline *pipe =
                q.blend == QuadBlend::Opaque ? g.pipe_opaque :
                q.blend == QuadBlend::Alpha ? g.pipe_alpha : g.pipe_premul;
            SDL_BindGPUGraphicsPipeline(rp, pipe);
            SDL_GPUTextureSamplerBinding tsb{q.tex, q.linear ? g.samp_linear : g.samp_nearest};
            SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
            // the quad shader adds corner * size to both the position and the texture coordinate, so
            // with NDC +Y up (see px_to_ndc) the texture's first row (v = 0, the picture's top) has to
            // sit on the quad's upper edge: v runs src_v + src_vh at the bottom up to src_v at the top.
            const float v_ubo[8] = {q.dst_x, q.dst_y, q.dst_w, q.dst_h,
                                    q.src_u, q.src_v + q.src_vh, q.src_uw, -q.src_vh};
            const float f_ubo[4] = {q.force_opaque ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
            SDL_PushGPUVertexUniformData(cb, 0, v_ubo, sizeof(v_ubo));
            SDL_PushGPUFragmentUniformData(cb, 0, f_ubo, sizeof(f_ubo));
            SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
        }
        if (draw_data) {
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cb, rp);
        }
        SDL_EndGPURenderPass(rp);
    }

    SDL_SubmitGPUCommandBuffer(cb);
    g.submit_ms = ms_since(t0);
}

void gl_fini() {
    if (g.dev) {
        SDL_WaitForGPUIdle(g.dev);
        if (g.upload_mapped) {
            SDL_UnmapGPUTransferBuffer(g.dev, g.upload_buf);
            g.upload_mapped = nullptr;
        }
        if (g.upload_buf) { SDL_ReleaseGPUTransferBuffer(g.dev, g.upload_buf); g.upload_buf = nullptr; }
        if (g.samp_nearest) { SDL_ReleaseGPUSampler(g.dev, g.samp_nearest); g.samp_nearest = nullptr; }
        if (g.samp_linear) { SDL_ReleaseGPUSampler(g.dev, g.samp_linear); g.samp_linear = nullptr; }
        if (g.pipe_opaque) { SDL_ReleaseGPUGraphicsPipeline(g.dev, g.pipe_opaque); g.pipe_opaque = nullptr; }
        if (g.pipe_alpha) { SDL_ReleaseGPUGraphicsPipeline(g.dev, g.pipe_alpha); g.pipe_alpha = nullptr; }
        if (g.pipe_premul) { SDL_ReleaseGPUGraphicsPipeline(g.dev, g.pipe_premul); g.pipe_premul = nullptr; }
        if (g.vert_shader) { SDL_ReleaseGPUShader(g.dev, g.vert_shader); g.vert_shader = nullptr; }
        if (g.frag_shader) { SDL_ReleaseGPUShader(g.dev, g.frag_shader); g.frag_shader = nullptr; }
        if (g.window) SDL_ReleaseWindowFromGPUDevice(g.dev, g.window);
        SDL_DestroyGPUDevice(g.dev);
        g.dev = nullptr;
    }
    if (g.window) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    g.quads.clear();
    g.uploads.clear();
    g.upload_cap = 0;
    g.upload_used = 0;
}

GpuVulkanRenderer::GpuVulkanRenderer() {
    if (!g.dev) return;
    gpu3d_ = gpu3d_create_sdlgpu(g.dev);
    if (!gpu3d_) {
        g.error = "gpu3d_create_sdlgpu failed on Vulkan device";
        return;
    }
    gpu3d_->set_quality(gpu3d_quality::exact);
    ok_ = true;
}

GpuVulkanRenderer::~GpuVulkanRenderer() {
    shutdown();
}

void GpuVulkanRenderer::release_layer(LayerTex &lt) {
    if (lt.tex && g.dev) {
        SDL_ReleaseGPUTexture(g.dev, lt.tex);
    }
    lt.tex = nullptr;
    lt.w = lt.h = 0;
}

void GpuVulkanRenderer::ensure_layer(LayerTex &lt, int w, int h) {
    if (lt.tex && lt.w == w && lt.h == h) return;
    release_layer(lt);
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = uint32_t(w);
    tci.height = uint32_t(h);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    lt.tex = SDL_CreateGPUTexture(g.dev, &tci);
    lt.w = w;
    lt.h = h;
}

void GpuVulkanRenderer::upload_layer(LayerTex &lt, int w, int h, const uint32_t *pixels, size_t stride_px) {
    if (!pixels || w <= 0 || h <= 0) return;
    ensure_layer(lt, w, h);
    queue_texture_upload(lt.tex, w, h, pixels, stride_px);
}

void GpuVulkanRenderer::reset_caches() {
    last_2d_version_ = UINT64_MAX;
    last_sides_version_ = UINT64_MAX;
}

void GpuVulkanRenderer::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    ok_ = false;
    if (g.dev) SDL_WaitForGPUIdle(g.dev);
    if (gpu3d_) {
        gpu3d_->finish();
        gpu3d_.reset();
    }
    release_layer(layer_bd_);
    release_layer(layer_mid_);
    release_layer(layer_top_);
    release_layer(layer_cpu_);
    release_layer(layer_sides_);
}

void GpuVulkanRenderer::draw_frame(const model2a::picture_parts &parts, int w2d, AspectMode aspect,
                                   const gpu3d_poly *polys, size_t poly_count, const gpu3d_rams *rams) {
    if (!ok_) return;
    last_poly_count_ = poly_count;
    last_batch_count_ = 1;

    float vx = 0, vy = 0, vw = 1, vh = 1;
    compute_viewport(aspect, w2d, vx, vy, vw, vh);
    float nx = 0, ny = 0, nw = 2, nh = 2;
    px_to_ndc(vx, vy, vw, vh, nx, ny, nw, nh);

    const int H = model2a::HEIGHT;
    const int P = model2a::picture_parts::PAD;
    const bool upload_2d = (parts.version != last_2d_version_);
    if (upload_2d) {
        last_2d_version_ = parts.version;
        if (parts.backdrop) {
            if (parts.bd_unpadded) {
                upload_layer(layer_bd_, w2d, H, parts.backdrop, size_t(w2d));
            } else {
                const int bw = w2d + 2 * P;
                const int bh = H + 2 * P;
                upload_layer(layer_bd_, bw, bh, parts.backdrop, size_t(bw));
            }
        }
        if (parts.mid) {
            upload_layer(layer_mid_, w2d, H, parts.mid, size_t(w2d));
        }
        if (parts.top) {
            upload_layer(layer_top_, w2d, H, parts.top, size_t(w2d));
        }
    }

    if (parts.backdrop && layer_bd_.tex) {
        float u0 = 0.0f, v0 = 0.0f, uw = 1.0f, vh_uv = 1.0f;
        if (!parts.bd_unpadded) {
            const float bw = float(w2d + 2 * P);
            const float bh = float(H + 2 * P);
            u0 = float(P - parts.bdx) / bw;
            v0 = float(P - parts.bdy) / bh;
            uw = float(w2d) / bw;
            vh_uv = float(H) / bh;
        }
        g.quads.push_back({layer_bd_.tex, parts.filter, QuadBlend::Opaque,
                           nx, ny, nw, nh, u0, v0, uw, vh_uv, true});
    }

    if (parts.mid && layer_mid_.tex && parts.mid_y1 > parts.mid_y0) {
        g.quads.push_back({layer_mid_.tex, false, QuadBlend::Alpha,
                           nx, ny, nw, nh, 0.0f, 0.0f, 1.0f, 1.0f, false});
    }

    if (parts.gpu3d && parts.has3d && poly_count > 0 && rams && gpu3d_) {
        const int scale = std::max(1, int(std::lround(g.render_scale)));
        const int W3 = w2d * scale;
        const int H3 = H * scale;
        if (target_w_ != W3 || target_h_ != H3) {
            gpu3d_->make_targets(W3, H3, 3);
            target_w_ = W3;
            target_h_ = H3;
            target_idx_ = 0;
        }
        gpu3d_->set_rams(*rams);
        gpu3d_frame f{};
        f.w = W3;
        f.h = H3;
        f.scale = scale;
        f.ss = 1;
        f.edge = false;
        f.w2d = w2d;
        f.pad = P;
        f.filter = parts.filter;
        if (gpu3d_->submit(polys, poly_count, f, target_idx_)) {
            gpu3d_->settle();
            auto *tex3d = static_cast<SDL_GPUTexture *>(gpu3d_->target_handle(target_idx_));
            if (tex3d) {
                g.quads.push_back({tex3d, false, QuadBlend::Premul,
                                   nx, ny, nw, nh, 0.0f, 0.0f, 1.0f, 1.0f, false});
            }
            target_idx_ = (target_idx_ + 1) % 3;
        }
    }

    if (parts.top && layer_top_.tex && parts.top_y1 > parts.top_y0) {
        g.quads.push_back({layer_top_.tex, false, QuadBlend::Alpha,
                           nx, ny, nw, nh, 0.0f, 0.0f, 1.0f, 1.0f, false});
    }

    if (parts.sides > 0 && parts.side_px && aspect == AspectMode::Widescreen32) {
        const uint64_t sver = uint64_t(parts.sides) ^ (uint64_t(uintptr_t(parts.side_px)) << 8);
        if (sver != last_sides_version_) {
            last_sides_version_ = sver;
            upload_layer(layer_sides_, parts.sides * 2, H, parts.side_px, size_t(parts.sides * 2));
        }
        if (layer_sides_.tex) {
            const float side_w = vw * (float(parts.sides) / float(w2d));
            float lx = 0, ly = 0, lw = 0, lh = 0;
            px_to_ndc(vx, vy, side_w, vh, lx, ly, lw, lh);
            g.quads.push_back({layer_sides_.tex, false, QuadBlend::Opaque,
                               lx, ly, lw, lh, 0.0f, 0.0f, 0.5f, 1.0f, true});
            float rx = 0, ry = 0, rw = 0, rh = 0;
            px_to_ndc(vx + vw - side_w, vy, side_w, vh, rx, ry, rw, rh);
            g.quads.push_back({layer_sides_.tex, false, QuadBlend::Opaque,
                               rx, ry, rw, rh, 0.5f, 0.0f, 0.5f, 1.0f, true});
        }
    }
}

void GpuVulkanRenderer::draw_cpu_framebuffer(const uint32_t *argb, int w, int h, AspectMode aspect) {
    if (!ok_ || !argb || w <= 0 || h <= 0) return;
    last_poly_count_ = 0;
    last_batch_count_ = 1;
    upload_layer(layer_cpu_, w, h, argb, size_t(w));
    if (!layer_cpu_.tex) return;
    float vx = 0, vy = 0, vw = 1, vh = 1;
    compute_viewport(aspect, w, vx, vy, vw, vh);
    float nx = 0, ny = 0, nw = 2, nh = 2;
    px_to_ndc(vx, vy, vw, vh, nx, ny, nw, nh);
    g.quads.push_back({layer_cpu_.tex, true, QuadBlend::Opaque,
                       nx, ny, nw, nh, 0.0f, 0.0f, 1.0f, 1.0f, true});
}

} // namespace arm64
