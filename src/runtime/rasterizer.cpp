#include "runtime/rasterizer.hpp"

#include <algorithm>
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
                 const std::optional<RasterTexture> &texture)
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
                const auto column =
                    static_cast<std::uint32_t>(std::clamp(u, 0.0, static_cast<double>(texture->width - 1)));
                const auto row =
                    static_cast<std::uint32_t>(std::clamp(v, 0.0, static_cast<double>(texture->height - 1)));
                color =
                    memory.read_u32(GuestAddress{texture->address.value_of() + (row * texture->stride + column) * 4});
            }
            const auto address = pixel_address(target, x, y);
            memory.write_u32(address, (color & 0xFFFFFFU) | (memory.read_u32(address) & 0xFF000000U));
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
