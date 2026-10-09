#include "frontend/window.hpp"

#include "runtime/display.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace psp::frontend
{

namespace
{

void require(bool success)
{
    if (!success)
    {
        throw std::runtime_error(std::string("SDL: ") + SDL_GetError());
    }
}

} // namespace

Window::Video::Video()
{
    require(SDL_Init(SDL_INIT_VIDEO));
}

Window::Video::~Video()
{
    SDL_Quit();
}

Window::Window()
    : window_(SDL_CreateWindow("PSPEmulator", 960, 544, SDL_WINDOW_RESIZABLE), SDL_DestroyWindow),
      renderer_(nullptr, SDL_DestroyRenderer), texture_(nullptr, SDL_DestroyTexture)
{
    require(window_ != nullptr);
    renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
    require(renderer_ != nullptr);
    require(SDL_SetRenderVSync(renderer_.get(), 0));
    require(SDL_SetRenderLogicalPresentation(renderer_.get(), detail::Display::width, detail::Display::height,
                                             SDL_LOGICAL_PRESENTATION_LETTERBOX));
    texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                     detail::Display::width, detail::Display::height));
    require(texture_ != nullptr);
    require(SDL_SetTextureScaleMode(texture_.get(), SDL_SCALEMODE_NEAREST));
    require(SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE));
    const Payload black(std::size_t{detail::Display::width} * detail::Display::height * 4, 0);
    update(black);
}

void Window::update(std::span<const std::uint8_t> pixels)
{
    if (pixels.size() != std::size_t{detail::Display::width} * detail::Display::height * 4)
    {
        throw std::invalid_argument("Expected a packed 480x272 RGBA frame");
    }
    require(SDL_UpdateTexture(texture_.get(), nullptr, pixels.data(), detail::Display::width * 4));
    present();
}

bool Window::poll()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        handle(event);
    }
    if (open_ && redraw_)
    {
        present();
    }
    return open_;
}

bool Window::wait_until(std::chrono::steady_clock::time_point deadline)
{
    while (poll())
    {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
        {
            return true;
        }
        const auto timeout = std::min(std::chrono::ceil<std::chrono::milliseconds>(remaining).count(),
                                      std::chrono::milliseconds::rep{10});
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, static_cast<int>(timeout)))
        {
            handle(event);
        }
    }
    return false;
}

void Window::handle(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE))
    {
        open_ = false;
    }
    if (event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_RESIZED)
    {
        redraw_ = true;
    }
}

void Window::present()
{
    require(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255));
    require(SDL_RenderClear(renderer_.get()));
    require(SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr));
    require(SDL_RenderPresent(renderer_.get()));
    redraw_ = false;
}

int run_windowed(Execution &execution, Window &window)
{
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t frames = 0;
    bool finished = false;
    int exit_code = 0;
    while (window.poll())
    {
        if (finished)
        {
            window.wait_until(std::chrono::steady_clock::now() + std::chrono::seconds{1});
            continue;
        }
        if (execution.advance() == ExecutionEvent::Finished)
        {
            window.update(execution.pixels());
            exit_code = execution.result().exit_code;
            finished = true;
            continue;
        }
        ++frames;
        // Fixed-origin rational deadlines avoid accumulated rounding and host refresh drift.
        const auto deadline = start + std::chrono::nanoseconds{frames * 1'001'000'000 / 60};
        if (!window.wait_until(deadline))
        {
            break;
        }
        if (std::chrono::steady_clock::now() - deadline < std::chrono::nanoseconds{1'001'000'000 / 60})
        {
            window.update(execution.pixels());
        }
    }
    return exit_code;
}

} // namespace psp::frontend
