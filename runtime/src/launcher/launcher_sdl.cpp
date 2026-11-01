#ifndef _WIN32
#include "launcher_ui.h"
#include "runtime/log.h"

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdlib>
#include <vector>

namespace launcher {

namespace {

SDL_Renderer* g_renderer = nullptr;
std::vector<SDL_Texture*> g_textures;

ui::Image upload(const unsigned char* rgba, int w, int h) {
    SDL_Texture* t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return {};
    SDL_UpdateTexture(t, nullptr, rgba, w * 4);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    g_textures.push_back(t);
    return {reinterpret_cast<ImTextureID>(t), w, h};
}

}

Result run() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOG_WARN("launcher", "SDL init failed: %s", SDL_GetError());
        return Result::Play;
    }
    SDL_Window* window = SDL_CreateWindow("God of War: Ascension", 1120, 640, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    g_renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!g_renderer) {
        LOG_WARN("launcher", "launcher window failed: %s", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        return Result::Play;
    }
    SDL_SetRenderVSync(g_renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForSDLRenderer(window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    ui::setup(upload);
    Result result = Result::Quit;
    for (bool done = false; !done;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                result = Result::Quit;
                done = true;
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(20);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (ui::frame(result)) done = true;
        ImGui::Render();
        SDL_SetRenderDrawColor(g_renderer, 12, 7, 6, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        static const char* shot = std::getenv("GOWA_LAUNCHER_SHOT");
        static int frames = 0;
        if (shot && ++frames == 30) {
            if (SDL_Surface* sf = SDL_RenderReadPixels(g_renderer, nullptr)) {
                SDL_SaveBMP(sf, shot);
                SDL_DestroySurface(sf);
            }
            result = Result::Quit;
            done = true;
        }
        SDL_RenderPresent(g_renderer);
    }
    ui::shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    for (SDL_Texture* t : g_textures) SDL_DestroyTexture(t);
    g_textures.clear();
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
    SDL_DestroyWindow(window);
    return result;
}

}
#endif
