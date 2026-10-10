#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

namespace psp
{

// The PSP LCD panel: 480 x 272 visible pixels, refreshed at 60000/1001 Hz as given
// by PSPSDK's pspdisplay.h. Guest vblanks and host frame pacing share this cadence.
inline constexpr int kLcdWidth = 480;
inline constexpr int kLcdHeight = 272;
using LcdFrames = std::chrono::duration<std::int64_t, std::ratio<1001, 60'000>>;

} // namespace psp
