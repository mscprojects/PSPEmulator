#include "frontend/window.hpp"

#include "loader/tests/prx_fixture.hpp"
#include "runtime/tests/triangle_fixture.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

namespace psp::frontend
{

namespace
{

SDL_Window *only_window()
{
    int count = 0;
    auto **windows = SDL_GetWindows(&count);
    auto *window = count == 1 ? windows[0] : nullptr;
    SDL_free(static_cast<void *>(windows));
    return window;
}

// SDL timer callbacks only enqueue host events; rendering and guest execution stay on the main thread.
Uint32 close_after_delay(void *, SDL_TimerID, Uint32)
{
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_ESCAPE;
    SDL_PushEvent(&event);
    return 0;
}

} // namespace

TEST(WindowTest, RendersGuestPixelsWithNearestScalingAndPreservesAspectRatioAfterResize)
{
    std::ifstream input(std::string(PSPEMU_RUNTIME_FIXTURES_ROOT) + "/screen_hello_world.prx", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const Payload bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Execution execution(read_prx(bytes));
    while (execution.advance() != ExecutionEvent::Finished)
    {
    }
    Window window;
    ASSERT_TRUE(window.poll());
    window.update(execution.pixels());
    auto *native_window = only_window();
    ASSERT_NE(native_window, nullptr);
    auto *renderer = SDL_GetRenderer(native_window);
    ASSERT_NE(renderer, nullptr);
    const auto check_pixels = [&](int output_height, int top)
    {
        // Readback clips to the logical viewport. Include the letterbox bars;
        // the isolated software renderer retains its presented surface.
        ASSERT_TRUE(SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
        const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> raw(SDL_RenderReadPixels(renderer, nullptr),
                                                                              SDL_DestroySurface);
        ASSERT_TRUE(SDL_SetRenderLogicalPresentation(renderer, 480, 272, SDL_LOGICAL_PRESENTATION_LETTERBOX));
        ASSERT_NE(raw, nullptr) << SDL_GetError();
        const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rgba(
            SDL_ConvertSurface(raw.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        ASSERT_NE(rgba, nullptr) << SDL_GetError();
        ASSERT_EQ(rgba->w, 960);
        ASSERT_EQ(rgba->h, output_height);
        const auto *pixels = static_cast<const std::uint8_t *>(rgba->pixels);
        for (int y = 0; y < output_height; ++y)
        {
            for (int x = 0; x < 960; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    std::uint8_t expected = 0;
                    if (y >= top && y < top + 544)
                    {
                        expected = execution.pixels()[((y - top) / 2 * 480 + x / 2) * 4 + channel];
                    }
                    ASSERT_EQ(pixels[y * rgba->pitch + x * 4 + channel], expected)
                        << "at " << x << "," << y << " channel " << channel;
                }
            }
        }
    };
    check_pixels(544, 0);
    ASSERT_TRUE(SDL_SetWindowSize(native_window, 960, 960));
    ASSERT_TRUE(SDL_SyncWindow(native_window));
    ASSERT_TRUE(window.poll());
    check_pixels(960, 208);
}

TEST(WindowTest, EscapeCloseAndQuitEventsEndTheFrontend)
{
    for (const auto type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_WINDOW_CLOSE_REQUESTED, SDL_EVENT_QUIT})
    {
        Window window;
        SDL_Event event{};
        event.type = type;
        event.key.key = SDLK_ESCAPE;
        ASSERT_TRUE(SDL_PushEvent(&event));
        EXPECT_FALSE(window.wait_until(std::chrono::steady_clock::now() + std::chrono::seconds{1}));
    }
}

TEST(WindowTest, FinalFrameStaysOpenUntilEscapeAndKeepsGuestExitStatus)
{
    test::PrxFixture fixture;
    fixture.word(0x104, 0x03E00008); // jr $ra after loading v0 = 7
    fixture.word(0x108, 0);
    Execution execution(read_prx(fixture.bytes));
    Window window;
    const auto timer = SDL_AddTimer(100, close_after_delay, nullptr);
    ASSERT_NE(timer, 0U);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(run_windowed(execution, window), 7);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{80});
    EXPECT_EQ(execution.result().instructions_executed, 3U);
}

TEST(WindowTest, EscapeDuringActiveExecutionStopsBeforeTheInstructionBudget)
{
    test::PrxFixture fixture;
    fixture.word(0x100, 0x1000FFFF); // Infinite guest branch and nop delay slot.
    Execution execution(read_prx(fixture.bytes));
    Window window;
    const auto timer = SDL_AddTimer(60, close_after_delay, nullptr);
    ASSERT_NE(timer, 0U);
    EXPECT_EQ(run_windowed(execution, window), 0);
    EXPECT_GT(execution.guest_time(), 0U);
    EXPECT_LT(execution.guest_time(), 50'000'000U);
    EXPECT_THROW(execution.result(), std::logic_error);
}

TEST(WindowTest, KeyboardButtonsAnalogOppositesReleaseAndFocusLoss)
{
    Window window;
    ASSERT_TRUE(window.poll());
    const auto key = [&](SDL_Scancode scancode, bool down)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.scancode = scancode;
        ASSERT_TRUE(SDL_PushEvent(&event));
        ASSERT_TRUE(window.poll());
    };
    EXPECT_EQ(window.controller(), ControllerState{});
    constexpr std::array buttons{SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT,
                                 SDL_SCANCODE_DOWN,      SDL_SCANCODE_LEFT,   SDL_SCANCODE_Q,  SDL_SCANCODE_E,
                                 SDL_SCANCODE_I,         SDL_SCANCODE_L,      SDL_SCANCODE_K,  SDL_SCANCODE_J};
    constexpr std::array masks{0x1U,   0x8U,   0x10U,   0x20U,   0x40U,   0x80U,
                               0x100U, 0x200U, 0x1000U, 0x2000U, 0x4000U, 0x8000U};
    for (std::size_t index = 0; index < buttons.size(); ++index)
    {
        key(buttons[index], true);
        EXPECT_EQ(window.controller().buttons, masks[index]);
        key(buttons[index], false);
        EXPECT_EQ(window.controller().buttons, 0U);
    }
    key(SDL_SCANCODE_UP, true);
    key(SDL_SCANCODE_K, true);
    key(SDL_SCANCODE_W, true);
    key(SDL_SCANCODE_D, true);
    EXPECT_EQ(window.controller(), (ControllerState{0x4010, 255, 0}));
    key(SDL_SCANCODE_A, true);
    key(SDL_SCANCODE_S, true);
    EXPECT_EQ(window.controller(), (ControllerState{0x4010, 128, 128}));
    key(SDL_SCANCODE_D, false);
    key(SDL_SCANCODE_W, false);
    EXPECT_EQ(window.controller(), (ControllerState{0x4010, 0, 255}));
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    ASSERT_TRUE(SDL_PushEvent(&event));
    ASSERT_TRUE(window.poll());
    EXPECT_EQ(window.controller(), ControllerState{});
}

TEST(WindowTest, HomeQueuesOneGuestExitRequestAndIgnoresKeyRepeat)
{
    Window window;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_HOME;
    ASSERT_TRUE(SDL_PushEvent(&event));
    ASSERT_TRUE(window.poll());
    EXPECT_TRUE(window.take_exit_request());
    EXPECT_FALSE(window.take_exit_request());
    event.key.repeat = true;
    ASSERT_TRUE(SDL_PushEvent(&event));
    ASSERT_TRUE(window.poll());
    EXPECT_FALSE(window.take_exit_request());
    EXPECT_EQ(window.controller(), ControllerState{});
}

TEST(WindowTest, TriangleFinishesThroughGuestCallbacksAndRetainsExpectedPixelsUntilEscape)
{
    std::ifstream input(std::string(PSPEMU_RUNTIME_FIXTURES_ROOT) + "/triangle.prx", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const Payload bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Execution execution(read_prx(bytes));
    Window window;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_HOME;
    ASSERT_TRUE(SDL_PushEvent(&event));
    const auto timer = SDL_AddTimer(500, close_after_delay, nullptr);
    ASSERT_NE(timer, 0U);
    EXPECT_EQ(run_windowed(execution, window), 0);
    SDL_RemoveTimer(timer);
    EXPECT_EQ(execution.result().exit_code, 0);
    const auto expected = test::triangle_pixels();
    EXPECT_EQ(Payload(execution.pixels().begin(), execution.pixels().end()), expected);
    auto *native_window = only_window();
    ASSERT_NE(native_window, nullptr);
    auto *renderer = SDL_GetRenderer(native_window);
    ASSERT_NE(renderer, nullptr);
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> raw(SDL_RenderReadPixels(renderer, nullptr),
                                                                          SDL_DestroySurface);
    ASSERT_NE(raw, nullptr) << SDL_GetError();
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rgba(
        SDL_ConvertSurface(raw.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    ASSERT_NE(rgba, nullptr) << SDL_GetError();
    ASSERT_EQ(rgba->w, 960);
    ASSERT_EQ(rgba->h, 544);
    const auto *pixels = static_cast<const std::uint8_t *>(rgba->pixels);
    for (int y = 0; y < rgba->h; ++y)
    {
        for (int x = 0; x < rgba->w; ++x)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                ASSERT_EQ(pixels[y * rgba->pitch + x * 4 + channel], expected[(y / 2 * 480 + x / 2) * 4 + channel])
                    << "at " << x << "," << y << " channel " << channel;
            }
        }
    }
}

TEST(WindowTest, ControllerSampleExitsThroughHomeAndRetainsWindowUntilEscape)
{
    std::ifstream input(std::string(PSPEMU_RUNTIME_FIXTURES_ROOT) + "/controller_basic.prx", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const Payload bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ExecutionOptions options;
    options.max_instructions = 2'000'000;
    Execution execution(read_prx(bytes), options);
    Window window;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_HOME;
    ASSERT_TRUE(SDL_PushEvent(&event));
    // Escape is a delayed host close; Home must finish the guest before that close.
    const auto timer = SDL_AddTimer(1500, close_after_delay, nullptr);
    ASSERT_NE(timer, 0U);
    EXPECT_EQ(run_windowed(execution, window), 0);
    SDL_RemoveTimer(timer);
    EXPECT_EQ(execution.result().exit_code, 0);
    EXPECT_GE(execution.guest_time(), 16684U);
}

} // namespace psp::frontend
