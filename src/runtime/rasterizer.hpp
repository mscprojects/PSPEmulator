#pragma once

#include "memory/memory.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace psp::detail
{

// Screen position in 1/16 pixels and packed RGBA 8888 color.
struct RasterVertex
{
    std::int32_t x;
    std::int32_t y;
    std::uint32_t color;
    float u{};
    float v{};
};

// Linear RGBA8888 texture. The caller validates the stride, dimensions, and full range.
struct RasterTexture
{
    GuestAddress address;
    std::uint32_t stride;
    int width;
    int height;
};

// RGBA 8888 framebuffer with an inclusive pixel clip rectangle. The caller validates
// the complete stride x kLcdHeight range and keeps the clip inside the stride and display.
struct RenderTarget
{
    GuestAddress framebuffer;
    std::uint32_t stride;
    int left;
    int top;
    int right;
    int bottom;
};

enum class Shading : std::uint8_t
{
    Flat,
    Smooth,
};

enum class ClearChannels : std::uint8_t
{
    Color,
    ColorAndAlpha,
};

// Pixel-center coverage with a top-left shared-edge rule. Flat shading uses the last
// vertex color; smooth shading interpolates each channel with integer barycentrics.
// Degenerate triangles write nothing.
void draw_triangle(Memory &memory, const RenderTarget &target, std::array<RasterVertex, 3> vertices, Shading shading);
// Through-mode sprite with nearest sampling, clamp addressing, and replacement RGB.
// Mixed corner orientations rotate the UV axes; framebuffer alpha/stencil is preserved.
void draw_sprite(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                 const std::optional<RasterTexture> &texture);
// Fill the clipped rectangle spanned by two corners with the second corner's color.
// Color-only clears preserve each pixel's existing alpha.
void draw_clear(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                ClearChannels channels);

} // namespace psp::detail
