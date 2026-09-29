#include "core/memory.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

namespace
{

TEST(MemoryTest, ReadsAndWritesLittleEndianValues)
{
    psp::Memory memory(0x08800000, 8);
    EXPECT_EQ(memory.read_u32(0x08800000), 0U);
    memory.write_u32(0x08800000, 0x12345678);

    EXPECT_EQ(memory.read_u8(0x08800000), 0x78);
    EXPECT_EQ(memory.read_u8(0x08800001), 0x56);
    EXPECT_EQ(memory.read_u16(0x08800000), 0x5678);
    EXPECT_EQ(memory.read_u32(0x08800000), 0x12345678U);

    memory.write_u16(0x08800002, 0xABCD);
    EXPECT_EQ(memory.read_u32(0x08800000), 0xABCD5678U);
}

TEST(MemoryTest, SupportsUnalignedAccess)
{
    psp::Memory memory(0x08800000, 8);
    memory.write_u32(0x08800001, 0x12345678);

    EXPECT_EQ(memory.read_u32(0x08800001), 0x12345678U);
    EXPECT_EQ(memory.read_u16(0x08800002), 0x3456);
}

TEST(MemoryTest, AllowsLastByteAndRejectsCrossBoundaryAccess)
{
    psp::Memory memory(0x08800000, 4);
    memory.write_u8(0x08800003, 0xA5);
    EXPECT_EQ(memory.read_u8(0x08800003), 0xA5);

    EXPECT_THROW(memory.read_u8(0x087FFFFF), std::out_of_range);
    EXPECT_THROW(memory.read_u16(0x08800003), std::out_of_range);
    EXPECT_THROW(memory.write_u32(0x08800001, 1), std::out_of_range);
    EXPECT_EQ(memory.read_u8(0x08800003), 0xA5);
}

TEST(MemoryTest, AcceptsRegionEndingAtLastAddress)
{
    psp::Memory memory(0xFFFFFFFE, 2);
    memory.write_u16(0xFFFFFFFE, 0x1234);
    EXPECT_EQ(memory.read_u8(0xFFFFFFFF), 0x12);
}

TEST(MemoryTest, RejectsInvalidRegions)
{
    EXPECT_THROW(psp::Memory(0, 0), std::invalid_argument);
    EXPECT_THROW(psp::Memory(0xFFFFFFFF, 2), std::invalid_argument);
}

} // namespace
