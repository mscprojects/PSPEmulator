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
