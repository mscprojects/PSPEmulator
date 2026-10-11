#include "runtime/rasterizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace psp::detail
{

namespace
{

GuestAddress pixel_address(const RenderTarget &target, int x, int y)
{
    return GuestAddress{target.framebuffer.value_of() +
                        (static_cast<std::uint32_t>(y) * target.stride + static_cast<std::uint32_t>(x)) * 4};
}

// Both operands are packed PSP colors; their order follows the source/destination blend equation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::uint32_t blend_rgb(std::uint32_t source, std::uint32_t destination, const RasterBlend &blend)
{
    const auto source_alpha = static_cast<int>(source >> 24);
    const auto destination_alpha = static_cast<int>(destination >> 24);
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
    {
        const auto source_color = static_cast<int>((source >> shift) & 255);
        const auto destination_color = static_cast<int>((destination >> shift) & 255);
        const auto factor = [&](BlendFactor value, bool source_factor)
        {
            const auto other = source_factor ? destination_color : source_color;
            switch (value)
            {
            case BlendFactor::OtherColor:
                return other;
            case BlendFactor::OneMinusOtherColor:
                return 255 - other;
            case BlendFactor::SourceAlpha:
                return source_alpha;
            case BlendFactor::OneMinusSourceAlpha:
                return 255 - source_alpha;
            case BlendFactor::DestinationAlpha:
                return destination_alpha;
            case BlendFactor::OneMinusDestinationAlpha:
                return 255 - destination_alpha;
            case BlendFactor::DoubleSourceAlpha:
                return 2 * source_alpha;
            case BlendFactor::OneMinusDoubleSourceAlpha:
                return 255 - 2 * source_alpha;
            case BlendFactor::DoubleDestinationAlpha:
                return 2 * destination_alpha;
            case BlendFactor::OneMinusDoubleDestinationAlpha:
                return 255 - 2 * destination_alpha;
            case BlendFactor::Fixed:
                return static_cast<int>(((source_factor ? blend.source_fixed : blend.destination_fixed) >> shift) &
                                        255);
            }
            std::unreachable();
        };
        // PSP multiplies channel/factor midpoints and truncates each term separately.
        // Doubled factors may exceed 255; doubled inverse factors may be negative.
        // See the pinned software blender reference in docs/runtime.md.
        const auto weighted = [](int color, int weight)
        {
            const auto magnitude = ((2 * color + 1) * (2 * std::abs(weight) + 1)) / 1024;
            return weight < 0 ? -magnitude : magnitude;
        };
        int value = 0;
        switch (blend.operation)
        {
        case BlendOperation::Add:
            value = weighted(source_color, factor(blend.source_factor, true)) +
                    weighted(destination_color, factor(blend.destination_factor, false));
            break;
        case BlendOperation::Subtract:
            value = weighted(source_color, factor(blend.source_factor, true)) -
                    weighted(destination_color, factor(blend.destination_factor, false));
            break;
        case BlendOperation::ReverseSubtract:
            value = weighted(destination_color, factor(blend.destination_factor, false)) -
                    weighted(source_color, factor(blend.source_factor, true));
            break;
        case BlendOperation::Minimum:
            value = std::min(source_color, destination_color);
            break;
        case BlendOperation::Maximum:
            value = std::max(source_color, destination_color);
            break;
        case BlendOperation::AbsoluteDifference:
            value = std::abs(source_color - destination_color);
            break;
        }
        result |= static_cast<std::uint32_t>(std::clamp(value, 0, 255)) << shift;
    }
    return result;
}

std::uint32_t sample_texture(const Memory &memory, const RasterTexture &texture, double u, double v)
{
    const auto texel = [&](int x, int y)
    {
        const auto column = static_cast<std::uint32_t>(std::clamp(x, 0, texture.width - 1));
        const auto row = static_cast<std::uint32_t>(std::clamp(y, 0, texture.height - 1));
        return memory.read_u32(GuestAddress{texture.address.value_of() + (row * texture.stride + column) * 4});
    };
    // Clamping before conversion bounds integer coordinates; each tap still clamps independently.
    const auto bounded_u = std::clamp(u, 0.0, static_cast<double>(texture.width));
    const auto bounded_v = std::clamp(v, 0.0, static_cast<double>(texture.height));
    if (texture.filter == TextureFilter::Nearest)
    {
        return texel(static_cast<int>(bounded_u), static_cast<int>(bounded_v));
    }
    // GE quantizes to 1/16 texel, shifts to texel centers, then truncates each horizontal
    // blend before the vertical blend. See the raster pipeline reference in docs/runtime.md.
    const auto fixed_u = static_cast<int>(bounded_u * 16) - 8;
    const auto fixed_v = static_cast<int>(bounded_v * 16) - 8;
    const auto x = fixed_u >> 4;
    const auto y = fixed_v >> 4;
    const auto horizontal = static_cast<std::uint32_t>(fixed_u & 15);
    const auto vertical = static_cast<std::uint32_t>(fixed_v & 15);
    const std::array colors{texel(x, y), texel(x + 1, y), texel(x, y + 1), texel(x + 1, y + 1)};
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        const auto top =
            (((colors[0] >> shift) & 255) * (16 - horizontal) + ((colors[1] >> shift) & 255) * horizontal) >> 4;
        const auto bottom =
            (((colors[2] >> shift) & 255) * (16 - horizontal) + ((colors[3] >> shift) & 255) * horizontal) >> 4;
        result |= ((top * (16 - vertical) + bottom * vertical) >> 4) << shift;
    }
    return result;
}

} // namespace

void draw_triangle(Memory &memory, const RenderTarget &target, std::array<RasterVertex, 3> vertices, Shading shading)
{
    const auto flat_color = vertices[2].color;
    const auto edge = [](const RasterVertex &a, const RasterVertex &b, std::int32_t x, std::int32_t y)
    { return std::int64_t{b.x - a.x} * (y - a.y) - std::int64_t{b.y - a.y} * (x - a.x); };
    auto area = edge(vertices[0], vertices[1], vertices[2].x, vertices[2].y);
    if (area == 0)
    {
        return;
    }
    if (area < 0)
    {
        std::swap(vertices[1], vertices[2]);
        area = -area;
    }
    const auto top_left = [](const RasterVertex &a, const RasterVertex &b)
    { return b.y < a.y || (b.y == a.y && b.x > a.x); };
    const std::array inclusive{top_left(vertices[1], vertices[2]), top_left(vertices[2], vertices[0]),
                               top_left(vertices[0], vertices[1])};
    // Exact PSP subpixel coverage and fixed-point color-plane rounding require
    // hardware probes in a later milestone.
    for (int y = target.top; y <= target.bottom; ++y)
    {
        for (int x = target.left; x <= target.right; ++x)
        {
            const std::array weights{edge(vertices[1], vertices[2], x * 16 + 8, y * 16 + 8),
                                     edge(vertices[2], vertices[0], x * 16 + 8, y * 16 + 8),
                                     edge(vertices[0], vertices[1], x * 16 + 8, y * 16 + 8)};
            if (weights[0] < 0 || weights[1] < 0 || weights[2] < 0 || (weights[0] == 0 && !inclusive[0]) ||
                (weights[1] == 0 && !inclusive[1]) || (weights[2] == 0 && !inclusive[2]))
            {
                continue;
            }
            auto color = flat_color;
            if (shading == Shading::Smooth)
            {
                color = 0;
                for (unsigned channel = 0; channel < 4; ++channel)
                {
                    std::int64_t value = 0;
                    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex)
                    {
                        value += weights[vertex] * ((vertices[vertex].color >> (channel * 8)) & 255);
                    }
                    color |= static_cast<std::uint32_t>(value / area) << (channel * 8);
                }
            }
            memory.write_u32(pixel_address(target, x, y), color);
        }
    }
}

void draw_sprite(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                 const std::optional<RasterTexture> &texture, const SpriteOperations &operations)
{
    const auto &first = vertices[0];
    const auto &second = vertices[1];
    if (first.x == second.x || first.y == second.y)
    {
        return;
    }
    const bool rotated = (first.x > second.x) != (first.y > second.y);
    // PSP sprites use asymmetric start rounding; rotated sprites round both axes alike.
    const auto left = std::max(target.left, (std::min(first.x, second.x) + 6) >> 4);
    const auto top = std::max(target.top, (std::min(first.y, second.y) + (rotated ? 6 : 7)) >> 4);
    const auto right = std::min(target.right + 1, (std::max(first.x, second.x) + 7) >> 4);
    const auto bottom = std::min(target.bottom + 1, (std::max(first.y, second.y) + 7) >> 4);
    // Exact hardware reciprocal/gradient rounding for non-power-of-two extents is
    // still provisional. precisionnearest2d covers power-of-two scales and UV offsets.
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            auto color = second.color;
            if (texture)
            {
                const auto horizontal = static_cast<double>(x * 16 + 8 - first.x) / (second.x - first.x);
                const auto vertical = static_cast<double>(y * 16 + 8 - first.y) / (second.y - first.y);
                const auto u = first.u + (static_cast<double>(second.u) - first.u) * (rotated ? vertical : horizontal);
                const auto v = first.v + (static_cast<double>(second.v) - first.v) * (rotated ? horizontal : vertical);
                const auto sampled = sample_texture(memory, *texture, u, v);
                color = 0;
                for (unsigned shift = 0; shift < 24; shift += 8)
                {
                    auto channel = ((sampled >> shift) & 255) * (texture->double_color ? 2U : 1U);
                    if (texture->function == TextureFunction::Modulate)
                    {
                        // Texture multiplication uses a vertex-color bias, unlike framebuffer blending.
                        // Double before truncation so half * half * 2 yields 127, not 126.
                        channel = (channel * (((second.color >> shift) & 255) + 1)) >> 8;
                    }
                    color |= std::min(channel, 255U) << shift;
                }
                auto alpha = second.color >> 24;
                if (texture->use_alpha)
                {
                    alpha = sampled >> 24;
                    if (texture->function == TextureFunction::Modulate)
                    {
                        alpha = (alpha * ((second.color >> 24) + 1)) >> 8;
                    }
                }
                color |= alpha << 24;
            }
            const auto address = pixel_address(target, x, y);
            const auto destination = memory.read_u32(address);
            const auto rgb = operations.blend ? blend_rgb(color, destination, *operations.blend) : color & 0xFFFFFFU;
            const auto stencil = operations.stencil_reference ? std::uint32_t{*operations.stencil_reference} << 24
                                                              : destination & 0xFF000000U;
            memory.write_u32(address, rgb | stencil);
        }
    }
}

void draw_clear(Memory &memory, const RenderTarget &target, const std::array<RasterVertex, 2> &vertices,
                ClearChannels channels)
{
    const auto left = std::max(target.left, (std::min(vertices[0].x, vertices[1].x) + 15) / 16);
    const auto top = std::max(target.top, (std::min(vertices[0].y, vertices[1].y) + 15) / 16);
    const auto right = std::min(target.right + 1, (std::max(vertices[0].x, vertices[1].x) + 15) / 16);
    const auto bottom = std::min(target.bottom + 1, (std::max(vertices[0].y, vertices[1].y) + 15) / 16);
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            const auto address = pixel_address(target, x, y);
            auto color = vertices[1].color;
            if (channels == ClearChannels::Color)
            {
                color = (color & 0xFFFFFFU) | (memory.read_u32(address) & 0xFF000000U);
            }
            memory.write_u32(address, color);
        }
    }
}

} // namespace psp::detail
