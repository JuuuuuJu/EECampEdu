#include "CameraViewerWin.hpp"

#include <algorithm>
#include <cstdint>

CameraViewerWin::~CameraViewerWin() {
    ResetTexture();
}

void CameraViewerWin::ResetTexture() {
    if (preview_texture != nullptr) {
        SDL_DestroyTexture(preview_texture);
        preview_texture = nullptr;
    }
    texture_width = 0;
    texture_height = 0;
}

void CameraViewerWin::draw(AppState& state, SDL_Renderer* renderer) {
    ImGui::Begin("Camera / USB Monitor");

    ImGui::Text("CDC image frames received: %d", state.received_image_count);
    if (!state.last_frame_header.empty()) {
        ImGui::TextWrapped("Last frame: %s", state.last_frame_header.c_str());
    } else {
        ImGui::TextWrapped("No CDC image frame has been seen yet.");
    }

    if (state.latest_frame_dirty && !state.latest_frame_rgba.empty()) {
        if (preview_texture == nullptr || texture_width != state.latest_frame_width || texture_height != state.latest_frame_height) {
            ResetTexture();
            preview_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, state.latest_frame_width, state.latest_frame_height);
            texture_width = state.latest_frame_width;
            texture_height = state.latest_frame_height;
        }
        if (preview_texture != nullptr) {
            SDL_UpdateTexture(preview_texture, nullptr, state.latest_frame_rgba.data(), state.latest_frame_width * 4);
            state.latest_frame_dirty = false;
        }
    }

    ImGui::SeparatorText("Preview");
    if (preview_texture != nullptr) {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float scale = std::max(1.0f, std::min(available_width / static_cast<float>(texture_width), 4.0f));
        ImGui::Image(reinterpret_cast<ImTextureID>(preview_texture), ImVec2(texture_width * scale, texture_height * scale));
    } else if (state.latest_frame_format == 4) {
        ImGui::TextWrapped("Image frame received, but no decoded texture is available yet. Check the serial log for decode errors.");
    } else {
        ImGui::TextWrapped("Waiting for a CDC image frame.");
    }

    ImGui::SeparatorText("Serial log");
    ImGui::BeginChild("usb_log", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(state.usb_rx_log.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

