#include "runtime/display.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

namespace psp::detail
{

TEST(DisplayTest, CapturesRgbaWithOpaqueAlphaAndGuestStrideThroughVramAlias)
{
    Memory memory(GuestAddress{0x04000000}, 0x200000);
    memory.map_alias(GuestAddress{0x44000000}, GuestAddress{0x04000000});
    Display display(memory);
    memory.write_u32(GuestAddress{0x04000000}, 0x00123456);
    memory.write_u32(GuestAddress{0x04000000 + 480 * 4}, 0xFFFFFFFF); // Invisible row padding.
    memory.write_u32(GuestAddress{0x04000000 + 512 * 4}, 0x78452301);
    memory.write_u32(GuestAddress{0x04000000 + (271 * 512 + 479) * 4}, 0x00ABCDEF);
    display.set_mode(0, 480, 272);
    display.set_framebuffer({GuestAddress{0x44000000}, 512, 3}, 0);
    display.capture();
    const auto pixels = display.pixels();
    EXPECT_EQ(pixels.size(), 480U * 272 * 4);
    EXPECT_EQ((std::array{pixels[0], pixels[1], pixels[2], pixels[3]}),
              (std::array<std::uint8_t, 4>{0x56, 0x34, 0x12, 0xFF}));
    EXPECT_EQ((std::array{pixels[std::size_t{480} * 4], pixels[std::size_t{480} * 4 + 1],
                          pixels[std::size_t{480} * 4 + 2], pixels[std::size_t{480} * 4 + 3]}),
              (std::array<std::uint8_t, 4>{1, 0x23, 0x45, 0xFF}));
    EXPECT_EQ(pixels[std::size_t{479} * 4], 0);
    EXPECT_EQ(pixels[pixels.size() - 4], 0xEF);
    EXPECT_EQ(pixels[pixels.size() - 3], 0xCD);
    EXPECT_EQ(pixels[pixels.size() - 2], 0xAB);
    EXPECT_EQ(pixels.back(), 0xFF);
    // Captured bytes remain stable until the next capture.
    memory.write_u32(GuestAddress{0x04000000}, 0);
    EXPECT_EQ(display.pixels()[0], 0x56);
    display.capture();
    EXPECT_EQ(display.pixels()[0], 0);
}

TEST(DisplayTest, NextFrameSelectionActivatesOnlyAtVblankAndNullDisablesIt)
{
    Memory memory(GuestAddress{0x04000000}, 0x200000);
    Display display(memory);
    memory.write_u32(GuestAddress{0x04000000}, 0x00000011);
    memory.write_u32(GuestAddress{0x04100000}, 0x00000022);
    display.set_framebuffer({GuestAddress{0x04000000}, 512, 3}, 1);
    display.capture();
    EXPECT_EQ(display.pixels()[0], 0);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0x11);
    display.set_framebuffer({GuestAddress{0x04100000}, 512, 3}, 1);
    display.capture();
    EXPECT_EQ(display.pixels()[0], 0x11);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0x22);
    display.set_framebuffer({GuestAddress{0}, 0, 3}, 1);
    display.capture();
    EXPECT_EQ(display.pixels()[0], 0x22);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0);
    EXPECT_EQ(display.pixels()[3], 255);
}

TEST(DisplayTest, LatestSelectionWinsAndImmediateSelectionCancelsPendingChange)
{
    Memory memory(GuestAddress{0x04000000}, 0x200000);
    Display display(memory);
    memory.write_u32(GuestAddress{0x04000000}, 0x11);
    memory.write_u32(GuestAddress{0x04100000}, 0x22);
    display.set_framebuffer({GuestAddress{0x04000000}, 512, 3}, 1);
    display.set_framebuffer({GuestAddress{0x04100000}, 512, 3}, 1);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0x22);
    display.set_framebuffer({GuestAddress{0x04100000}, 512, 3}, 1);
    display.set_framebuffer({GuestAddress{0x04000000}, 512, 3}, 0);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0x11);
}

TEST(DisplayTest, RejectsUnsupportedModesAndInvalidFramebufferRangesWithoutReplacingSelection)
{
    Memory memory(GuestAddress{0x04000000}, 0x200000);
    Display display(memory);
    memory.write_u32(GuestAddress{0x04000000}, 0x77);
    display.set_framebuffer({GuestAddress{0x04000000}, 512, 3}, 1);
    EXPECT_THROW(display.set_mode(1, 480, 272), std::runtime_error);
    EXPECT_THROW(display.set_mode(0, 640, 272), std::runtime_error);
    EXPECT_THROW(display.set_mode(0, 480, 480), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000000}, 512, 0}, 0), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000000}, 512, 3}, 2), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000001}, 512, 3}, 0), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000000}, 256, 3}, 0), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000000}, 513, 3}, 0), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x041FFFF0}, 512, 3}, 0), std::out_of_range);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0xFFFFFFF0}, 512, 3}, 0), std::runtime_error);
    EXPECT_THROW(display.set_framebuffer({GuestAddress{0x04000000}, 0x80000000, 3}, 0), std::runtime_error);
    display.vblank();
    EXPECT_EQ(display.pixels()[0], 0x77);
}

} // namespace psp::detail
