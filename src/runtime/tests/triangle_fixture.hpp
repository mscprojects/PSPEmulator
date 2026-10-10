#pragma once

#include "common/payload.hpp"

namespace psp::test
{

// Analytic scanline expectation for the sample's (240,40), (80,232),
// (400,232) vertices. At height t=(y+0.5-40)/192, the horizontal span is
// 240 +/- 160*t. Red is 1-t; green/blue split t by horizontal position.
// Rational arithmetic avoids rounding ambiguity and uses no renderer edge code.
inline Payload triangle_pixels()
{
    Payload pixels(std::size_t{480} * 272 * 4);
    for (int y = 0; y < 272; ++y)
    {
        for (int x = 0; x < 480; ++x)
        {
            const auto offset = static_cast<std::size_t>((y * 480 + x) * 4);
            pixels[offset + 3] = 255;
            const auto height = 2 * y - 79;
            const auto horizontal = 2 * x - 479;
            const auto red = 10 * (384 - height);
            const auto green = 5 * height - 6 * horizontal;
            const auto blue = 5 * height + 6 * horizontal;
            if (height >= 0 && red > 0 && green > 0 && blue >= 0)
            {
                pixels[offset] = static_cast<std::uint8_t>(255 * red / 3840);
                pixels[offset + 1] = static_cast<std::uint8_t>(255 * green / 3840);
                pixels[offset + 2] = static_cast<std::uint8_t>(255 * blue / 3840);
            }
        }
    }
    return pixels;
}

} // namespace psp::test
