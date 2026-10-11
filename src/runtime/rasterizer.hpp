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

enum class TextureFilter : std::uint8_t
{
    Nearest,
    Linear,
};

enum class TextureFunction : std::uint8_t
{
    Modulate,
    Replace,
};

enum class BlendOperation : std::uint8_t
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
    AbsoluteDifference,
};

// OtherColor means destination RGB for the source factor, source RGB for the destination factor.
enum class BlendFactor : std::uint8_t
{
    OtherColor,
    OneMinusOtherColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    DoubleSourceAlpha,
    OneMinusDoubleSourceAlpha,
    DoubleDestinationAlpha,
    OneMinusDoubleDestinationAlpha,
    Fixed,
};

struct RasterBlend
{
    BlendOperation operation;
    BlendFactor source_factor;
    BlendFactor destination_factor;
    std::uint32_t source_fixed{};
    std::uint32_t destination_fixed{};
};

// Only ALWAYS/REPLACE stencil is supported; absent reference preserves the framebuffer alpha byte.
struct SpriteOperations
{
    std::optional<RasterBlend> blend{};
    std::optional<std::uint8_t> stencil_reference{};
};

// Non-swizzled RGBA8888 texture. RGB functions retain vertex alpha when use_alpha is false.
// Color doubling applies before RGB truncation and clamping; alpha is never doubled.
// The caller validates the stride, dimensions, and full range.
struct RasterTexture
{
    GuestAddress address;
    std::uint32_t stride;
    int width;
    int height;
    TextureFilter filter{TextureFilter::Nearest};
    bool use_alpha{true};
    TextureFunction function{TextureFunction::Replace};
    bool double_color{};
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
// Through-mode sprite with nearest or linear sampling, clamp addressing, and MODULATE/REPLACE texturing.
// Mixed corner orientations rotate the UV axes. Blend factors read the original framebuffer;
// stencil replacement happens after RGB blending. The caller validates the operation and texture enums.
void draw_sprite(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                 const std::optional<RasterTexture> &texture, const SpriteOperations &operations = {});
// Fill the clipped rectangle spanned by two corners with the second corner's color.
// Color-only clears preserve each pixel's existing alpha.
void draw_clear(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                ClearChannels channels);

} // namespace psp::detail
