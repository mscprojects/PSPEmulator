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

TEST_F(RasterizerTest, GivenPaddedTexture_WhenLinearSpriteMagnifies_ThenBlendsTexelsAndPreservesStencil)
{
    memory.write_u32(GuestAddress{0x04100000}, 0xFFFFFFFF);
    memory.write_u32(GuestAddress{0x04100004}, 0xFF000000);
    memory.write_u32(GuestAddress{0x04100008}, 0x12345678); // Row padding must not enter the filter.
    memory.write_u32(GuestAddress{0x04100010}, 0xFF0000FF);
    memory.write_u32(GuestAddress{0x04100014}, 0xFF00FF00);
    const RasterTexture texture{GuestAddress{0x04100000}, 4, 2, 2, TextureFilter::Linear};
    for (unsigned index = 0; index < 16; ++index)
        memory.write_u32(GuestAddress{0x04000000 + (index / 4 * 512 + index % 4) * 4}, 0x44000000);
    draw_sprite(memory, target, {{{0, 0, 0, 0, 0}, {64, 64, 0, 2, 2}}}, texture);
    // Texel centers lie at 0.5 and 1.5; the interior samples use 1/4 and 3/4 weights.
    constexpr std::array<std::uint32_t, 16> expected{
        0x44FFFFFF, 0x44BFBFBF, 0x443F3F3F, 0x44000000, 0x44BFBFFF, 0x448F9FBF, 0x442F5F3F, 0x44003F00,
        0x443F3FFF, 0x442F5FBF, 0x440F9F3F, 0x4400BF00, 0x440000FF, 0x44003FBF, 0x4400BF3F, 0x4400FF00};
    std::array<std::uint32_t, 16> actual{};
    for (unsigned index = 0; index < actual.size(); ++index)
        actual[index] = pixel(static_cast<int>(index % 4), static_cast<int>(index / 4));
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(pixel(4, 0), 0U);
    EXPECT_EQ(pixel(0, 4), 0U);
}

TEST_F(RasterizerTest, GivenLinearModulateTexture_WhenDrawingSprite_ThenFiltersBeforeModulatingEachChannel)
{
    memory.write_u32(GuestAddress{0x04100000}, 0x01030101);
    memory.write_u32(GuestAddress{0x04100004}, 0x03010303);
    memory.write_u32(GuestAddress{0x04000000}, 0x44000000);
    const RasterTexture texture{.address = GuestAddress{0x04100000},
                                .stride = 2,
                                .width = 2,
                                .height = 1,
                                .filter = TextureFilter::Linear,
                                .function = TextureFunction::Modulate};
    draw_sprite(memory, target, {{{0, 0, 0xFFFFFFFF, 1, 0.5F}, {16, 16, 0x7F7F7F7F, 1, 0.5F}}}, texture);
    // Filtering 1 and 3 first produces 2, then modulation produces 1.
    // Modulating the taps first would produce 0 and 1, whose average truncates to 0.
    EXPECT_EQ(pixel(0, 0), 0x44010101U);
    EXPECT_EQ(pixel(1, 0), 0U);
    // Expose the filtered, modulated alpha through a white destination scaled by source alpha.
    memory.write_u32(GuestAddress{0x04000000}, 0xFFFFFFFF);
    const SpriteOperations operations{.blend = RasterBlend{.operation = BlendOperation::Add,
                                                           .source_factor = BlendFactor::Fixed,
                                                           .destination_factor = BlendFactor::SourceAlpha}};
    draw_sprite(memory, target, {{{0, 0, 0xFFFFFFFF, 1, 0.5F}, {16, 16, 0x7F7F7F7F, 1, 0.5F}}}, texture, operations);
    EXPECT_EQ(pixel(0, 0), 0xFF010101U);
}

TEST_F(RasterizerTest, GivenFixedBlendFactors_WhenDrawingSprite_ThenWeightsEachRgbChannelIndependently)
{
    memory.write_u32(GuestAddress{0x04000000}, 0x50604020);
    const SpriteOperations operations{.blend = RasterBlend{.operation = BlendOperation::Add,
                                                           .source_factor = BlendFactor::Fixed,
                                                           .destination_factor = BlendFactor::Fixed,
                                                           .source_fixed = 0xFF8000,
                                                           .destination_fixed = 0x0080FF}};
    draw_sprite(memory, target, {at(0, 0, 0), at(1, 1, 0x90C08040)}, std::nullopt, operations);
    // Red takes the destination, blue takes the source, green adds two half-weighted terms.
    EXPECT_EQ(pixel(0, 0), 0x50C06020U);
    EXPECT_EQ(pixel(1, 0), 0U);
}

TEST_F(RasterizerTest, GivenNegativeDoubledInverseAlpha_WhenBlending_ThenKeepsSignedTermsUntilFinalClamp)
{
    // Source alpha 144 gives inverse-double factor -33. Its midpoint product with
    // source channel 128 contributes -16; the destination contributes +64.
    const std::array operations{BlendOperation::Add, BlendOperation::Subtract, BlendOperation::ReverseSubtract};
    constexpr std::array expected{0x44303030U, 0x44000000U, 0x44505050U};
    for (std::size_t index = 0; index < operations.size(); ++index)
    {
        SCOPED_TRACE(index);
        memory.write_u32(GuestAddress{0x04000000}, 0x44404040);
        const SpriteOperations state{.blend = RasterBlend{.operation = operations[index],
                                                          .source_factor = BlendFactor::OneMinusDoubleSourceAlpha,
                                                          .destination_factor = BlendFactor::Fixed,
                                                          .destination_fixed = 0xFFFFFF}};
        draw_sprite(memory, target, {at(0, 0, 0), at(1, 1, 0x90808080)}, std::nullopt, state);
        EXPECT_EQ(pixel(0, 0), expected[index]);
    }
}

TEST_F(RasterizerTest, GivenDoubledAlpha_WhenSubtracting_ThenClampsAfterCombiningWeightedTerms)
{
    memory.write_u32(GuestAddress{0x04000000}, 0x44A0A0A0);
    const SpriteOperations operations{.blend = RasterBlend{.operation = BlendOperation::Subtract,
                                                           .source_factor = BlendFactor::DoubleSourceAlpha,
                                                           .destination_factor = BlendFactor::Fixed,
                                                           .destination_fixed = 0xFFFFFF}};
    draw_sprite(memory, target, {at(0, 0, 0), at(1, 1, 0xFFC0C0C0)}, std::nullopt, operations);
    // The unclamped source contributes 383 and the destination 160: result 223.
    EXPECT_EQ(pixel(0, 0), 0x44DFDFDFU);
}

TEST_F(RasterizerTest, GivenFractionalUv_WhenLinearSampling_ThenQuantizesToFourBitWeights)
{
    memory.write_u32(GuestAddress{0x04100000}, 0);
    memory.write_u32(GuestAddress{0x04100004}, 0xFFFFFFFF);
    const RasterTexture texture{GuestAddress{0x04100000}, 2, 2, 1, TextureFilter::Linear};
    constexpr std::array coordinates{1.0F, 1.0624F, 1.0625F, 1.125F};
    constexpr std::array<std::uint32_t, 4> expected{0x007F7F7F, 0x007F7F7F, 0x008F8F8F, 0x009F9F9F};
    for (std::size_t index = 0; index < coordinates.size(); ++index)
    {
        SCOPED_TRACE(coordinates[index]);
        const auto u = coordinates[index];
        draw_sprite(memory, target, {{{0, 0, 0, u, 0.5F}, {16, 16, 0, u, 0.5F}}}, texture);
        EXPECT_EQ(pixel(0, 0), expected[index]);
    }
}

TEST_F(RasterizerTest, WhenLinearSamplingAtTexelMidpoint_ThenTruncatesHorizontalBlendsBeforeVerticalBlend)
{
    memory.write_u32(GuestAddress{0x04100000}, 0xFF000000);
    memory.write_u32(GuestAddress{0x04100004}, 0xFF010101);
    memory.write_u32(GuestAddress{0x04100008}, 0xFF010101);
    memory.write_u32(GuestAddress{0x0410000C}, 0xFF020202);
    const RasterTexture texture{GuestAddress{0x04100000}, 2, 2, 2, TextureFilter::Linear};
    memory.write_u32(target.framebuffer, 0x44808080);
    draw_sprite(memory, target, {{{0, 0, 0, 1, 1}, {16, 16, 0, 1, 1}}}, texture);
    // Horizontal blends truncate to 0 and 1, so their vertical blend truncates to 0.
    // A single four-tap average would incorrectly produce 1 in every channel.
    EXPECT_EQ(pixel(0, 0), 0x44000000U);
}

TEST_F(RasterizerTest, GivenBorderTexels_WhenLinearUvExceedsBounds_ThenClampsEveryTap)
{
    // Exactly the texture is mapped: filtering outside its borders must not read past it.
    memory.map_region(GuestAddress{0x08000000}, 16);
    memory.write_u32(GuestAddress{0x08000000}, 0xFFFFFFFF);
    memory.write_u32(GuestAddress{0x08000004}, 0xFF000000);
    memory.write_u32(GuestAddress{0x08000008}, 0xFF0000FF);
    memory.write_u32(GuestAddress{0x0800000C}, 0xFF00FF00);
    const RasterTexture texture{GuestAddress{0x08000000}, 2, 2, 2, TextureFilter::Linear};
    draw_sprite(memory, target, {{{0, 0, 0, -1, -1}, {64, 64, 0, 3, 3}}}, texture);
    EXPECT_EQ(pixel(0, 0), 0x00FFFFFFU);
    EXPECT_EQ(pixel(3, 0), 0U);
    EXPECT_EQ(pixel(0, 3), 0x000000FFU);
    EXPECT_EQ(pixel(3, 3), 0x0000FF00U);
}

TEST_F(RasterizerTest, GivenSingleTexel_WhenLinearUvIsFarOutsideBounds_ThenSamplesThatTexel)
{
    memory.map_region(GuestAddress{0x08000000}, 4);
    memory.write_u32(GuestAddress{0x08000000}, 0x12345678);
    const RasterTexture texture{GuestAddress{0x08000000}, 1, 1, 1, TextureFilter::Linear};
    draw_sprite(memory, target, {{{0, 0, 0, -65535, 65535}, {32, 32, 0, 65535, -65535}}}, texture);
    EXPECT_EQ(pixel(0, 0), 0x00345678U);
    EXPECT_EQ(pixel(1, 0), 0x00345678U);
    EXPECT_EQ(pixel(0, 1), 0x00345678U);
    EXPECT_EQ(pixel(1, 1), 0x00345678U);
}

TEST_F(RasterizerTest, GivenClippedLinearSprite_WhenDrawing_ThenKeepsOriginalUvPlaneAndPreservesOutsidePixels)
{
    memory.map_region(GuestAddress{0x08000000}, 16);
    memory.write_u32(GuestAddress{0x08000000}, 0xFFFFFFFF);
    memory.write_u32(GuestAddress{0x08000004}, 0xFF000000);
    memory.write_u32(GuestAddress{0x08000008}, 0xFF0000FF);
    memory.write_u32(GuestAddress{0x0800000C}, 0xFF00FF00);
    const RasterTexture texture{GuestAddress{0x08000000}, 2, 2, 2, TextureFilter::Linear};
    target.left = 1;
    target.top = 1;
    target.right = 2;
    target.bottom = 2;
    const Payload original(0x200000, 0x44);
    memory.write_bytes(target.framebuffer, original);
    draw_sprite(memory, target, {{{0, 0, 0, 0, 0}, {64, 64, 0, 2, 2}}}, texture);
    EXPECT_EQ(pixel(1, 1), 0x448F9FBFU);
    EXPECT_EQ(pixel(2, 1), 0x442F5F3FU);
    EXPECT_EQ(pixel(1, 2), 0x442F5FBFU);
    EXPECT_EQ(pixel(2, 2), 0x440F9F3FU);
    // Restore the four permitted writes and compare all other VRAM bytes.
    for (unsigned y = 1; y <= 2; ++y)
        for (unsigned x = 1; x <= 2; ++x)
            memory.write_u32(GuestAddress{0x04000000 + (y * 512 + x) * 4}, 0x44444444);
    EXPECT_EQ(frame(), original);
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
