#pragma once

#include "runtime/execution.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <memory>

namespace psp::frontend
{

// All SDL operations, including event handling and waiting, run on the main thread.
class Window
{
    struct Video
    {
        Video();
        ~Video();
    };

public:
    Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    void update(std::span<const std::uint8_t> pixels);
    bool poll();
    // Keeps handling events and repainting while waiting on the monotonic host clock.
    bool wait_until(std::chrono::steady_clock::time_point deadline);

private:
    void handle(const SDL_Event &event);
    void present();

    Video video_;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_;
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_;
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture_;
    bool open_{true};
    bool redraw_{true};
};

// Pace guest vblanks at 60000/1001 Hz; retain the final frame until close/Escape.
// Closure during execution returns zero without running further guest instructions.
int run_windowed(Execution &execution, Window &window);

} // namespace psp::frontend
