#include "memory/memory.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace psp
{

static_assert(!std::is_convertible_v<std::uint32_t, GuestAddress>);
static_assert(!std::is_convertible_v<GuestAddress, std::uint32_t>);

TEST(MemoryTest, ReadsAndWritesLittleEndianValues)
{
    Memory memory(GuestAddress{0x08800000}, 8);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800000}), 0U);
    memory.write_u32(GuestAddress{0x08800000}, 0x12345678);

    EXPECT_EQ(memory.read_u8(GuestAddress{0x08800000}), 0x78);
    EXPECT_EQ(memory.read_u8(GuestAddress{0x08800001}), 0x56);
    EXPECT_EQ(memory.read_u16(GuestAddress{0x08800000}), 0x5678);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800000}), 0x12345678U);

    memory.write_u16(GuestAddress{0x08800002}, 0xABCD);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800000}), 0xABCD5678U);
}

TEST(MemoryTest, SupportsUnalignedAccess)
{
    Memory memory(GuestAddress{0x08800000}, 8);
    memory.write_u32(GuestAddress{0x08800001}, 0x12345678);

    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800001}), 0x12345678U);
    EXPECT_EQ(memory.read_u16(GuestAddress{0x08800002}), 0x3456);
}

TEST(MemoryTest, AllowsLastByteAndRejectsCrossBoundaryAccess)
{
    Memory memory(GuestAddress{0x08800000}, 4);
    memory.write_u8(GuestAddress{0x08800003}, 0xA5);
    EXPECT_EQ(memory.read_u8(GuestAddress{0x08800003}), 0xA5);

    EXPECT_THROW(memory.read_u8(GuestAddress{0x087FFFFF}), std::out_of_range);
    EXPECT_THROW(memory.read_u16(GuestAddress{0x08800003}), std::out_of_range);
    EXPECT_THROW(memory.write_u32(GuestAddress{0x08800001}, 1), std::out_of_range);
    EXPECT_EQ(memory.read_u8(GuestAddress{0x08800003}), 0xA5);
}

TEST(MemoryTest, AcceptsRegionEndingAtLastAddress)
{
    Memory memory(GuestAddress{0xFFFFFFFE}, 2);
    memory.write_u16(GuestAddress{0xFFFFFFFE}, 0x1234);
    EXPECT_EQ(memory.read_u8(GuestAddress{0xFFFFFFFF}), 0x12);
}

TEST(MemoryTest, RejectsInvalidRegions)
{
    EXPECT_THROW(Memory(GuestAddress{0}, 0), std::invalid_argument);
    EXPECT_THROW(Memory(GuestAddress{0xFFFFFFFF}, 2), std::invalid_argument);
}

} // namespace psp
