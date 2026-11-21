// Menu-only Dear ImGui renderer drawing through ProjectDural's OpenGL ES 2.0
// draw list (gpu_gles.h). Dynamically scales to handheld displays (720x480,
// 640x480, 1280x720) with a liquid-chrome / electric-cyan Dural theme.
#pragma once

#include "gpu_gles.h"
#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace arm64 {

class ImGuiGles {
public:
    bool init() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendRendererName = "projectdural_gles2";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

        ImFontConfig font;
        font.SizePixels = gl_display_height() <= 480 ? 18.0f : 20.0f;
        io.Fonts->AddFontDefault(&font);
        unsigned char *pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        atlas_ = gl_create_ui_texture(w, h, pixels);
        if (!atlas_) {
            ImGui::DestroyContext();
            return false;
        }
        io.Fonts->SetTexID(ImTextureID(uintptr_t(atlas_)));

        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.FramePadding = {8.0f, 5.0f};
        style.ItemSpacing = {8.0f, 6.0f};
        style.ScrollbarSize = 18.0f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.13f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.68f, 0.90f, 0.45f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.45f, 0.72f, 1.0f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.52f, 0.80f, 0.85f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.68f, 0.96f, 0.95f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.44f, 0.68f, 0.85f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.65f, 0.92f, 0.95f);
        return true;
    }

    void frame(float dt) {
        auto &io = ImGui::GetIO();
        io.DisplaySize = {float(gl_display_width()), float(gl_display_height())};
        io.DeltaTime = std::clamp(dt, 0.001f, 0.1f);
        ImGui::NewFrame();
    }

    void render() {
        ImGui::Render();
        const auto *data = ImGui::GetDrawData();
        if (!data) return;
        for (const auto *list : data->CmdLists) {
            for (const auto &cmd : list->CmdBuffer) {
                if (cmd.UserCallback) {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                    continue;
                }
                const int x0 = int(cmd.ClipRect.x), y0 = int(cmd.ClipRect.y);
                const int x1 = int(cmd.ClipRect.z), y1 = int(cmd.ClipRect.w);
                if (x1 <= x0 || y1 <= y0 || !cmd.ElemCount) continue;
                vertices_.resize(cmd.ElemCount);
                for (unsigned i = 0; i < cmd.ElemCount; ++i) {
                    const auto &in = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]];
                    vertices_[i] = {in.pos.x, in.pos.y, in.uv.x, in.uv.y, in.col};
                }
                gl_ui_triangles(uint32_t(uintptr_t(cmd.GetTexID())), vertices_.data(), vertices_.size(),
                                x0, y0, x1, y1);
            }
        }
    }

    void shutdown() {
        if (atlas_) gl_delete_ui_texture(atlas_);
        atlas_ = 0;
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    }

private:
    uint32_t atlas_ = 0;
    std::vector<GlUiVertex> vertices_;
};

} // namespace arm64
