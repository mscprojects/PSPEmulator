#include "memory/memory.hpp"

#include <gtest/gtest.h>

#include <array>
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

TEST(MemoryTest, CopiesBinaryRangesWithIndependentOwnership)
{
    Memory memory(GuestAddress{100}, 4);
    const Payload expected{0, 0xFF, 'x', 0};
    memory.write_bytes(GuestAddress{100}, expected);
    const auto copy = memory.read_bytes(GuestAddress{100}, 4);
    EXPECT_EQ(copy, expected);
    EXPECT_EQ(memory.read_bytes(GuestAddress{101}, 2), (Payload{0xFF, 'x'}));
    memory.write_u8(GuestAddress{101}, 1);
    EXPECT_EQ(copy, expected);
    EXPECT_TRUE(memory.read_bytes(GuestAddress{0}, 0).empty());
    EXPECT_TRUE(memory.read_bytes(GuestAddress{104}, 0).empty());
    EXPECT_THROW(memory.read_bytes(GuestAddress{99}, 1), std::out_of_range);
    EXPECT_THROW(memory.read_bytes(GuestAddress{103}, 2), std::out_of_range);
    EXPECT_THROW(memory.read_bytes(GuestAddress{104}, 1), std::out_of_range);
}

TEST(MemoryTest, ReadsIntoCallerStorageAndRejectsInvalidRangesWithoutPartialCopies)
{
    Memory memory(GuestAddress{100}, 5);
    memory.write_bytes(GuestAddress{100}, Payload{0xA5, 0, 0xFF, 'x', 0});
    std::array<std::uint8_t, 4> destination{};
    const auto bytes = std::as_writable_bytes(std::span{destination});
    memory.read_into(GuestAddress{101}, bytes);
    const std::array<std::uint8_t, 4> expected{0, 0xFF, 'x', 0};
    EXPECT_EQ(destination, expected);
    EXPECT_THROW(memory.read_into(GuestAddress{99}, bytes), std::out_of_range);
    EXPECT_EQ(destination, expected);
    EXPECT_THROW(memory.read_into(GuestAddress{102}, bytes), std::out_of_range);
    EXPECT_EQ(destination, expected);
    EXPECT_NO_THROW(memory.read_into(GuestAddress{0}, {}));
    EXPECT_NO_THROW(memory.read_into(GuestAddress{105}, {}));

    Memory last_bytes(GuestAddress{0xFFFFFFFE}, 2);
    last_bytes.write_u16(GuestAddress{0xFFFFFFFE}, 0x1234);
    last_bytes.read_into(GuestAddress{0xFFFFFFFE}, bytes.first(2));
    EXPECT_EQ(destination, (std::array<std::uint8_t, 4>{0x34, 0x12, 'x', 0}));
    const auto original = destination;
    EXPECT_THROW(last_bytes.read_into(GuestAddress{0xFFFFFFFF}, bytes.first(2)), std::out_of_range);
    EXPECT_EQ(destination, original);
}

TEST(MemoryTest, ReadsStringsWithoutRequiringTheEntireLimitToBeMapped)
{
    Memory memory(GuestAddress{100}, 4);
    memory.write_bytes(GuestAddress{100}, Payload{'a', 'b', 'c', 0});
    EXPECT_EQ(memory.read_c_string(GuestAddress{100}, 4), "abc");
    EXPECT_EQ(memory.read_c_string(GuestAddress{101}, 4096), "bc");
    EXPECT_TRUE(memory.read_c_string(GuestAddress{103}, 1).empty());
    EXPECT_THROW(memory.read_c_string(GuestAddress{100}, 3), std::runtime_error);
    EXPECT_THROW(memory.read_c_string(GuestAddress{100}, 0), std::invalid_argument);
    EXPECT_THROW(memory.read_c_string(GuestAddress{99}, 4), std::out_of_range);
    EXPECT_THROW(memory.read_c_string(GuestAddress{104}, 1), std::out_of_range);
    memory.write_u8(GuestAddress{103}, 'd');
    EXPECT_THROW(memory.read_c_string(GuestAddress{100}, 5), std::out_of_range);
}

TEST(MemoryTest, ReadsRangesAndStringsAtTheEndOfTheAddressSpace)
{
    Memory memory(GuestAddress{0xFFFFFFFE}, 2);
    memory.write_bytes(GuestAddress{0xFFFFFFFE}, Payload{'x', 0});
    EXPECT_EQ(memory.read_bytes(GuestAddress{0xFFFFFFFE}, 2), (Payload{'x', 0}));
    EXPECT_EQ(memory.read_c_string(GuestAddress{0xFFFFFFFE}, 4096), "x");
    EXPECT_THROW(memory.read_bytes(GuestAddress{0xFFFFFFFF}, 2), std::out_of_range);
    memory.write_u8(GuestAddress{0xFFFFFFFF}, 'y');
    EXPECT_THROW(memory.read_c_string(GuestAddress{0xFFFFFFFE}, 4096), std::out_of_range);
}

TEST(MemoryTest, RegionsAreIndependentAndAliasesShareScalarAccesses)
{
    Memory memory(GuestAddress{0x08800000}, 8);
    memory.map_alias(GuestAddress{0x48800000}, GuestAddress{0x08800000});
    memory.map_alias(GuestAddress{0x88800000}, GuestAddress{0x08800000});
    memory.map_alias(GuestAddress{0xC8800000}, GuestAddress{0x88800000}); // Alias of an alias.
    memory.map_region(GuestAddress{0x04000000}, 8);
    memory.map_alias(GuestAddress{0x44000000}, GuestAddress{0x04000000});
    EXPECT_EQ(memory.read_u32(GuestAddress{0x44000000}), 0U);
    memory.write_u32(GuestAddress{0x48800001}, 0x12345678); // Unaligned writes still work.
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800001}), 0x12345678U);
    EXPECT_EQ(memory.read_u16(GuestAddress{0x88800002}), 0x3456U);
    memory.write_u8(GuestAddress{0xC8800004}, 0xAB);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x48800001}), 0xAB345678U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x04000001}), 0U);
    memory.write_u16(GuestAddress{0x44000002}, 0xCD12);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x04000000}), 0xCD120000U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x88800001}), 0xAB345678U);
}

TEST(MemoryTest, AliasesSupportByteRangesCallerStorageAndStrings)
{
    Memory memory(GuestAddress{0x04000000}, 5);
    memory.map_alias(GuestAddress{0x44000000}, GuestAddress{0x04000000});
    const Payload original{'a', 'b', 0, 0xFF, 0x55};
    memory.write_bytes(GuestAddress{0x44000000}, original);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, 5), original);
    EXPECT_EQ(memory.read_c_string(GuestAddress{0x44000000}, 4096), "ab");
    std::array<std::uint8_t, 3> destination{};
    memory.read_into(GuestAddress{0x44000002}, std::as_writable_bytes(std::span{destination}));
    EXPECT_EQ(destination, (std::array<std::uint8_t, 3>{0, 0xFF, 0x55}));
    EXPECT_THROW(memory.read_into(GuestAddress{0x44000003}, std::as_writable_bytes(std::span{destination})),
                 std::out_of_range);
    EXPECT_EQ(destination, (std::array<std::uint8_t, 3>{0, 0xFF, 0x55}));
    EXPECT_THROW(memory.write_bytes(GuestAddress{0x44000004}, Payload{1, 2}), std::out_of_range);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, 5), original);
    EXPECT_THROW(memory.read_c_string(GuestAddress{0x44000003}, 4096), std::out_of_range);
    EXPECT_THROW(memory.read_c_string(GuestAddress{0x44000000}, 2), std::runtime_error);
    EXPECT_TRUE(memory.read_bytes(GuestAddress{0xFFFFFFFF}, 0).empty());
    EXPECT_NO_THROW(memory.write_bytes(GuestAddress{0xFFFFFFFF}, {}));
    EXPECT_NO_THROW(memory.read_into(GuestAddress{0xFFFFFFFF}, {}));
}

TEST(MemoryTest, MappingRejectsOverlapsInvalidSourcesAndAddressWrapWithoutChangingData)
{
    Memory memory(GuestAddress{100}, 8);
    memory.write_u32(GuestAddress{100}, 0x12345678);
    memory.map_alias(GuestAddress{200}, GuestAddress{100});
    for (const auto base : {93U, 100U, 104U, 107U, 193U, 200U, 207U})
    {
        SCOPED_TRACE(base);
        EXPECT_THROW(memory.map_region(GuestAddress{base}, 8), std::invalid_argument);
        EXPECT_THROW(memory.map_alias(GuestAddress{base}, GuestAddress{100}), std::invalid_argument);
    }
    EXPECT_THROW(memory.map_alias(GuestAddress{300}, GuestAddress{101}), std::invalid_argument);
    EXPECT_THROW(memory.map_region(GuestAddress{300}, 0), std::invalid_argument);
    EXPECT_THROW(memory.map_region(GuestAddress{0xFFFFFFFC}, 8), std::invalid_argument);
    EXPECT_THROW(memory.map_alias(GuestAddress{0xFFFFFFFC}, GuestAddress{100}), std::invalid_argument);
    EXPECT_EQ(memory.read_u32(GuestAddress{100}), 0x12345678U);
    EXPECT_EQ(memory.read_u32(GuestAddress{200}), 0x12345678U);
    EXPECT_THROW(memory.read_u8(GuestAddress{300}), std::out_of_range);
    memory.map_region(GuestAddress{108}, 8); // Adjacent regions are allowed.
    memory.map_region(GuestAddress{0xFFFFFFF8}, 8);
    memory.map_alias(GuestAddress{0x40000000}, GuestAddress{0xFFFFFFF8});
    memory.write_u8(GuestAddress{0x40000007}, 0xAB);
    EXPECT_EQ(memory.read_u8(GuestAddress{0xFFFFFFFF}), 0xAB);
}

TEST(MemoryTest, AccessesCannotCrossRegionsOrAliasBoundaries)
{
    Memory memory(GuestAddress{100}, 4);
    memory.map_region(GuestAddress{104}, 4);
    memory.map_alias(GuestAddress{200}, GuestAddress{100});
    memory.map_alias(GuestAddress{204}, GuestAddress{104});
    memory.write_bytes(GuestAddress{100}, Payload{1, 2, 3, 4});
    memory.write_bytes(GuestAddress{104}, Payload{5, 6, 7, 8});
    for (const auto base : {100U, 200U})
    {
        SCOPED_TRACE(base);
        EXPECT_THROW(memory.read_u16(GuestAddress{base + 3}), std::out_of_range);
        EXPECT_THROW(memory.write_u32(GuestAddress{base + 1}, 0), std::out_of_range);
        EXPECT_THROW(memory.read_bytes(GuestAddress{base + 2}, 4), std::out_of_range);
        EXPECT_THROW(memory.write_bytes(GuestAddress{base + 3}, Payload{0, 0}), std::out_of_range);
        EXPECT_THROW(memory.read_c_string(GuestAddress{base}, 5), std::out_of_range);
        EXPECT_EQ(memory.read_bytes(GuestAddress{base}, 4), (Payload{1, 2, 3, 4}));
        EXPECT_EQ(memory.read_bytes(GuestAddress{base + 4}, 4), (Payload{5, 6, 7, 8}));
    }
    EXPECT_THROW(memory.read_u8(GuestAddress{99}), std::out_of_range);
    EXPECT_THROW(memory.read_u8(GuestAddress{108}), std::out_of_range);
    EXPECT_THROW(memory.read_u8(GuestAddress{199}), std::out_of_range);
    EXPECT_THROW(memory.read_u8(GuestAddress{208}), std::out_of_range);
}

TEST(MemoryTest, MemoryCopiesOwnTheirBytesAndKeepAliasesWithinTheCopy)
{
    Memory original(GuestAddress{0x08800000}, 4);
    original.map_region(GuestAddress{0x04000000}, 4);
    original.map_alias(GuestAddress{0x44000000}, GuestAddress{0x04000000});
    original.write_u32(GuestAddress{0x04000000}, 0x12345678);
    auto copy = original;
    copy.write_u32(GuestAddress{0x44000000}, 0xAAAAAAAA);
    EXPECT_EQ(copy.read_u32(GuestAddress{0x04000000}), 0xAAAAAAAAU);
    EXPECT_EQ(original.read_u32(GuestAddress{0x44000000}), 0x12345678U);
    // Adding regions must not invalidate alias references to existing storage.
    copy.map_region(GuestAddress{0x1000}, 4);
    copy.write_u32(GuestAddress{0x04000000}, 0xBBBBBBBB);
    EXPECT_EQ(copy.read_u32(GuestAddress{0x44000000}), 0xBBBBBBBBU);
    EXPECT_EQ(original.read_u32(GuestAddress{0x04000000}), 0x12345678U);
}

TEST(MemoryTest, RejectsInvalidRegions)
{
    EXPECT_THROW(Memory(GuestAddress{0}, 0), std::invalid_argument);
    EXPECT_THROW(Memory(GuestAddress{0xFFFFFFFF}, 2), std::invalid_argument);
}

TEST(MemoryTest, PspLayoutMapsVramAndSharedRamAddressViews)
{
    auto memory = create_psp_memory(GuestAddress{0x08800000}, 0x1000);
    memory.write_u32(GuestAddress{0x48800800}, 0x12345678);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800800}), 0x12345678U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x88800800}), 0x12345678U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0xC8800800}), 0x12345678U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x04000000}), 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x441FFFFC}), 0U);
    memory.write_u32(GuestAddress{0x44000000}, 0xAABBCCDD);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x04000000}), 0xAABBCCDDU);
    memory.write_u16(GuestAddress{0x041FFFFE}, 0x1234);
    EXPECT_EQ(memory.read_u16(GuestAddress{0x441FFFFE}), 0x1234U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x08800800}), 0x12345678U);
    for (const auto address : {0x03FFFFFFU, 0x04200000U, 0x04400000U, 0x04800000U, 0x44200000U, 0x84000000U,
                               0x08801000U, 0x48801000U, 0xA8800000U})
    {
        SCOPED_TRACE(address);
        EXPECT_THROW(memory.read_u8(GuestAddress{address}), std::out_of_range);
    }
    EXPECT_THROW(memory.write_u32(GuestAddress{0x441FFFFE}, 0), std::out_of_range);
    EXPECT_EQ(memory.read_u16(GuestAddress{0x041FFFFE}), 0x1234U);
}

TEST(MemoryTest, PspLayoutRejectsEmptyRamAndRamOutsideTheCanonicalRange)
{
    EXPECT_THROW(create_psp_memory(GuestAddress{0x08800000}, 0), std::invalid_argument);
    EXPECT_THROW(create_psp_memory(GuestAddress{0x48800000}, 0x1000), std::invalid_argument);
    EXPECT_THROW(create_psp_memory(GuestAddress{0x3FFFF000}, 0x2000), std::invalid_argument);
    EXPECT_THROW(create_psp_memory(GuestAddress{0x04000000}, 0x1000), std::invalid_argument);
    EXPECT_NO_THROW(create_psp_memory(GuestAddress{0x3FFFF000}, 0x1000));
}

} // namespace psp
