#include "../aurora_vita_backend.hpp"

#include <SDL3/SDL.h>
#include <vitaGL.h>

#include <cstdio>

namespace {
bool should_exit(const SDL_Event& event) noexcept {
  if (event.type == SDL_EVENT_QUIT) return true;
  if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
      event.gbutton.button == SDL_GAMEPAD_BUTTON_START) return true;
  return false;
}
}

int main() {
  std::printf("[aurora-vita] SDL3 + vitaGL coexistence probe\n");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
    std::printf("[aurora-vita] SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow("Aurora SDL3 + vitaGL", 960, 544, SDL_WINDOW_FULLSCREEN);
  if (!window) {
    std::printf("[aurora-vita] SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 2;
  }

  aurora::vita::BackendConfig config{};
  config.width = 960;
  config.height = 544;
  config.wait_vblank = true;
  config.diagnostics = false;
  if (!aurora::vita::initialize(config)) {
    std::printf("[aurora-vita] vitaGL backend init failed: %s\n",
                aurora::vita::last_init_failure_detail());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 3;
  }

  std::printf("[aurora-vita] SDL video=%s; vitaGL renderer initialized; START exits\n",
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown");

  bool running = true;
  uint64_t frame = 0;
  while (running) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (should_exit(event)) running = false;
    }

    if (!aurora::vita::begin_frame()) break;
    const float phase = static_cast<float>(frame % 180u) / 180.0f;
    glClearColor(0.08f + 0.30f * phase, 0.10f, 0.28f - 0.18f * phase, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    aurora::vita::end_frame();
    ++frame;
  }

  aurora::vita::shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
