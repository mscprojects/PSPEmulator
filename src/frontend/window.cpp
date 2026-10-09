#include "frontend/window.hpp"

#include "runtime/display.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

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

ControllerState Window::controller() const
{
    constexpr std::array bindings{
        std::pair{SDL_SCANCODE_BACKSPACE, 0x1U}, std::pair{SDL_SCANCODE_RETURN, 0x8U},
        std::pair{SDL_SCANCODE_UP, 0x10U},       std::pair{SDL_SCANCODE_RIGHT, 0x20U},
        std::pair{SDL_SCANCODE_DOWN, 0x40U},     std::pair{SDL_SCANCODE_LEFT, 0x80U},
        std::pair{SDL_SCANCODE_Q, 0x100U},       std::pair{SDL_SCANCODE_E, 0x200U},
        std::pair{SDL_SCANCODE_I, 0x1000U},      std::pair{SDL_SCANCODE_L, 0x2000U},
        std::pair{SDL_SCANCODE_K, 0x4000U},      std::pair{SDL_SCANCODE_J, 0x8000U},
    };
    ControllerState input;
    for (const auto &[key, button] : bindings)
    {
        if (pressed_.at(key))
        {
            input.buttons |= button;
        }
    }
    const auto axis = [&](SDL_Scancode negative, SDL_Scancode positive) -> std::uint8_t
    {
        if (pressed_.at(negative) == pressed_.at(positive))
        {
            return 128;
        }
        return pressed_.at(negative) ? 0 : 255;
    };
    input.left_x = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
    input.left_y = axis(SDL_SCANCODE_W, SDL_SCANCODE_S);
    return input;
}

bool Window::take_exit_request()
{
    return std::exchange(exit_requested_, false);
}

void Window::handle(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE))
    {
        open_ = false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
    {
        const auto key = static_cast<std::size_t>(event.key.scancode);
        if (key < pressed_.size())
        {
            pressed_[key] = event.type == SDL_EVENT_KEY_DOWN;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.key == SDLK_HOME || event.key.scancode == SDL_SCANCODE_HOME))
        {
            exit_requested_ = true;
        }
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        pressed_.fill(false);
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
        execution.set_controller(window.controller());
        if (window.take_exit_request())
        {
            execution.request_exit();
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
