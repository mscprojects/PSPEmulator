#include "runtime/rasterizer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace psp::detail
{

namespace
{

// Vertex positions are whole pixels converted to the rasterizer's 1/16-pixel units.
RasterVertex at(int x, int y, std::uint32_t color)
{
    return {x * 16, y * 16, color};
}

class RasterizerTest : public testing::Test
{
protected:
    std::uint32_t pixel(int x, int y) const
    {
        return memory.read_u32(GuestAddress{0x04000000U + static_cast<std::uint32_t>(y * 512 + x) * 4});
    }

    Payload frame() const
    {
        return memory.read_bytes(GuestAddress{0x04000000}, 0x200000);
    }

    Memory memory{GuestAddress{0x04000000}, 0x200000};
    RenderTarget target{
        .framebuffer = GuestAddress{0x04000000}, .stride = 512, .left = 0, .top = 0, .right = 479, .bottom = 271};
};

} // namespace

TEST_F(RasterizerTest, SmoothTriangleInterpolatesRgbaAndReversedWindingMatches)
{
    draw_triangle(memory, target, {at(0, 0, 0xFF0000FF), at(4, 0, 0xFF00FF00), at(0, 4, 0xFFFF0000)}, Shading::Smooth);
    EXPECT_EQ(pixel(0, 0), 0xFF1F1FBFU); // Weights 3/4, 1/8, 1/8.
    EXPECT_EQ(pixel(1, 1), 0xFF5F5F3FU);
    EXPECT_EQ(pixel(3, 0), 0U); // The diagonal belongs to the adjoining triangle.
    const auto original = frame();

    Memory reversed(GuestAddress{0x04000000}, 0x200000);
    draw_triangle(reversed, target, {at(0, 0, 0xFF0000FF), at(0, 4, 0xFFFF0000), at(4, 0, 0xFF00FF00)},
                  Shading::Smooth);
    EXPECT_EQ(reversed.read_bytes(GuestAddress{0x04000000}, 0x200000), original);
}

TEST_F(RasterizerTest, FlatTriangleUsesLastVertexAndDegenerateTriangleDoesNotWrite)
{
    // Reversed winding still takes the last submitted vertex color.
    draw_triangle(memory, target, {at(0, 0, 1), at(0, 4, 2), at(4, 0, 0xAABBCCDD)}, Shading::Flat);
    EXPECT_EQ(pixel(0, 0), 0xAABBCCDDU);
    const auto original = frame();
    draw_triangle(memory, target, {at(0, 0, 1), at(0, 4, 2), at(0, 8, 3)}, Shading::Flat);
    EXPECT_EQ(frame(), original);
}

TEST_F(RasterizerTest, TwoTrianglesShareAnEdgeWithoutHolesOrDoubleOwnership)
{
    const std::array first{at(0, 0, 1), at(4, 0, 1), at(0, 4, 1)};
    const std::array second{at(4, 0, 2), at(4, 4, 2), at(0, 4, 2)};
    draw_triangle(memory, target, first, Shading::Flat);
    draw_triangle(memory, target, second, Shading::Flat);
    for (int y = 0; y < 5; ++y)
    {
        for (int x = 0; x < 5; ++x)
        {
            EXPECT_EQ(pixel(x, y), x >= 4 || y >= 4 ? 0U : x + y < 3 ? 1U : 2U) << x << "," << y;
        }
    }
    // Reversing the draw order would change edge pixels if both triangles owned them.
    const auto original = frame();
    Memory reversed(GuestAddress{0x04000000}, 0x200000);
    draw_triangle(reversed, target, second, Shading::Flat);
    draw_triangle(reversed, target, first, Shading::Flat);
    EXPECT_EQ(reversed.read_bytes(GuestAddress{0x04000000}, 0x200000), original);
}

TEST_F(RasterizerTest, TrianglesWriteOnlyInsideTheInclusiveClipRectangle)
{
    target.left = 2;
    target.top = 1;
    target.right = 3;
    target.bottom = 2;
    draw_triangle(memory, target, {at(0, 0, 7), at(16, 0, 7), at(0, 16, 7)}, Shading::Flat);
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 5; ++x)
        {
            const bool inside = x >= 2 && x <= 3 && y >= 1 && y <= 2;
            EXPECT_EQ(pixel(x, y), inside ? 7U : 0U) << x << "," << y;
        }
    }
}

TEST_F(RasterizerTest, SpritesSamplePaddedTexturesInEveryCornerOrientationAndPreserveStencil)
{
    constexpr std::uint32_t white = 0x44FFFFFF;
    constexpr std::uint32_t black = 0x44000000;
    constexpr std::uint32_t red = 0x440000FF;
    constexpr std::uint32_t green = 0x4400FF00;
    memory.write_u32(GuestAddress{0x04100000}, 0xFFFFFFFF);
    memory.write_u32(GuestAddress{0x04100004}, 0xFF000000);
    memory.write_u32(GuestAddress{0x04100010}, 0xFF0000FF);
    memory.write_u32(GuestAddress{0x04100014}, 0xFF00FF00);
    const RasterTexture texture{GuestAddress{0x04100000}, 4, 2, 2};
    constexpr std::array<std::array<RasterVertex, 2>, 4> orientations{{
        {{{0, 0, 0, 0, 0}, {64, 32, 0, 2, 2}}},
        {{{64, 32, 0, 0, 0}, {0, 0, 0, 2, 2}}},
        {{{64, 0, 0, 0, 0}, {0, 32, 0, 2, 2}}},
        {{{0, 32, 0, 0, 0}, {64, 0, 0, 2, 2}}},
    }};
    constexpr std::array<std::array<std::uint32_t, 8>, 4> expected{{
        {white, white, black, black, red, red, green, green},
        {green, green, red, red, black, black, white, white},
        {red, red, white, white, green, green, black, black},
        {black, black, green, green, white, white, red, red},
    }};
    for (std::size_t orientation = 0; orientation < orientations.size(); ++orientation)
    {
        SCOPED_TRACE(orientation);
        for (unsigned index = 0; index < 8; ++index)
            memory.write_u32(GuestAddress{0x04000000 + (index / 4 * 512 + index % 4) * 4}, 0x44000000);
        draw_sprite(memory, target, orientations[orientation], texture);
        std::array<std::uint32_t, 8> actual{};
        for (unsigned index = 0; index < 8; ++index)
            actual[index] = pixel(static_cast<int>(index % 4), static_cast<int>(index / 4));
        EXPECT_EQ(actual, expected[orientation]);
        EXPECT_EQ(pixel(4, 0), 0U);
        EXPECT_EQ(pixel(0, 2), 0U);
    }
}

TEST_F(RasterizerTest, SpriteCoverageUsesSubpixelStartAndExclusiveEndAndClipsWrites)
{
    memory.write_u32(GuestAddress{0x04000000}, 0xAA000000);
    draw_sprite(memory, target, {{{9, 8, 0}, {40, 40, 0x12345678}}}, std::nullopt);
    EXPECT_EQ(pixel(0, 0), 0xAA345678U);
    EXPECT_EQ(pixel(1, 1), 0x00345678U);
    EXPECT_EQ(pixel(2, 0), 0U);
    EXPECT_EQ(pixel(0, 2), 0U);

    draw_sprite(memory, target, {{{10, 9, 0}, {48, 48, 0xFFABCDEF}}}, std::nullopt);
    EXPECT_EQ(pixel(0, 0), 0xAA345678U);
    EXPECT_EQ(pixel(1, 1), 0x00ABCDEFU);
    target.left = 2;
    target.top = 2;
    target.right = 2;
    target.bottom = 2;
    draw_sprite(memory, target, {at(0, 0, 0), at(4, 4, 0xFFFFFFFF)}, std::nullopt);
    EXPECT_EQ(pixel(2, 2), 0x00FFFFFFU);
    EXPECT_EQ(pixel(3, 2), 0U);
}

TEST_F(RasterizerTest, SpriteNearestSamplingClampsBothEndsOfTheTexture)
{
    memory.write_u32(GuestAddress{0x04100000}, 0xFFFFFFFF);
    memory.write_u32(GuestAddress{0x04100004}, 0xFF000000);
    memory.write_u32(GuestAddress{0x04100008}, 0xFF0000FF);
    memory.write_u32(GuestAddress{0x0410000C}, 0xFF00FF00);
    const RasterTexture texture{GuestAddress{0x04100000}, 2, 2, 2};
    draw_sprite(memory, target, {{{0, 0, 0, -1, -1}, {64, 64, 0, 3, 3}}}, texture);
    EXPECT_EQ(pixel(0, 0), 0x00FFFFFFU);
    EXPECT_EQ(pixel(3, 0), 0U);
    EXPECT_EQ(pixel(0, 3), 0x000000FFU);
    EXPECT_EQ(pixel(3, 3), 0x0000FF00U);
}

TEST_F(RasterizerTest, ClearFillsHalfOpenRectangleAndColorOnlyClearPreservesAlpha)
{
    for (const auto channels : {ClearChannels::Color, ClearChannels::ColorAndAlpha})
    {
        SCOPED_TRACE(static_cast<int>(channels));
        memory.write_u32(GuestAddress{0x04000000U + (512 + 1) * 4}, 0xAA000000);
        draw_clear(memory, target, {at(1, 1, 0), at(3, 2, 0x12345678)}, channels);
        EXPECT_EQ(pixel(1, 1), channels == ClearChannels::Color ? 0xAA345678U : 0x12345678U);
        EXPECT_EQ(pixel(2, 1), channels == ClearChannels::Color ? 0x00345678U : 0x12345678U);
        EXPECT_EQ(pixel(3, 1), 0U);
        EXPECT_EQ(pixel(1, 2), 0U);
        EXPECT_EQ(pixel(0, 1), 0U);
        memory.write_u32(GuestAddress{0x04000000U + (512 + 2) * 4}, 0);
    }
}

} // namespace psp::detail
