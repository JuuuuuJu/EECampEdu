#pragma once

#include "App/AppState.hpp"
#include "imgui.h"

#include <SDL3/SDL.h>

class CameraViewerWin {
public:
    ~CameraViewerWin();
    void draw(AppState& state, SDL_Renderer* renderer);
    void ResetTexture();

private:
    SDL_Texture* preview_texture = nullptr;
    int texture_width = 0;
    int texture_height = 0;
};