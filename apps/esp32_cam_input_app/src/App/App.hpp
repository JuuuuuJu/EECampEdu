#pragma once

#include "App/AppState.hpp"
#include "Windows/CameraViewerWin.hpp"
#include "Windows/ControlsWin.hpp"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

class App {
public:
    bool Init();
    void Update();
    void Render();
    void Shutdown();
    bool isDone() const;

private:
    bool done = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_WindowFlags window_flags = 0;
    ImGuiIO* io = nullptr;
    ImGuiStyle* style = nullptr;
    float main_scale = 1.0f;
    ImVec4 clear_color = ImVec4(0.05f, 0.07f, 0.08f, 1.00f);

    AppState app_state;
    CameraViewerWin camera_viewer;
    ControlsWin controls;
};
