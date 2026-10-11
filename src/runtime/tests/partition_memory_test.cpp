#include "memory/memory.hpp"
#include "runtime/partition_memory.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace psp::detail
{

TEST(PartitionMemoryTest, LowAndHighAllocationsShareCapacityAndFailuresPreserveIt)
{
    std::uint32_t next_id{1};
    AddressArena arena(0x1100, 0x1500);
    PartitionMemory partitions(arena, next_id);
    const auto low = partitions.allocate({2, 0, 1});
    const auto high = partitions.allocate({2, 1, 1});
    EXPECT_EQ(partitions.address(low), GuestAddress{0x1100});
    EXPECT_EQ(partitions.address(high), GuestAddress{0x1400});
    EXPECT_EQ(partitions.free_size(), 512U);
    EXPECT_THROW(partitions.allocate({2, 0, 513}), std::runtime_error);
    EXPECT_EQ(partitions.free_size(), 512U);
    const auto remaining = partitions.allocate({2, 0, 512});
    EXPECT_EQ(partitions.address(remaining), GuestAddress{0x1200});
    EXPECT_EQ(partitions.free_size(), 0U);
    EXPECT_THROW(partitions.allocate({2, 1, 1}), std::runtime_error);
}

TEST(PartitionMemoryTest, GivenLiveBlocks_WhenFreeOrAllocationSizeIsInvalid_ThenPreservesBlocks)
{
    Memory memory(GuestAddress{0x1000}, 1280);
    std::uint32_t next_id{1};
    AddressArena arena(0x1100, 0x1500);
    PartitionMemory partitions(arena, next_id);
    const auto block = partitions.allocate({2, 0, 257});
    const auto address = partitions.address(block);
    memory.write_u32(address, 0x12345678);
    EXPECT_THROW(partitions.free(0), std::out_of_range);
    EXPECT_THROW(partitions.free(0xDEADBEEF), std::out_of_range);
    for (const auto size : {0U, 513U, std::numeric_limits<std::uint32_t>::max()})
    {
        EXPECT_THROW(partitions.allocate({2, 0, size}), std::runtime_error);
        EXPECT_THROW(partitions.allocate({2, 1, size}), std::runtime_error);
    }
    EXPECT_EQ(partitions.free_size(), 512U);
    EXPECT_EQ(partitions.largest_free_size(), 512U);
    EXPECT_EQ(partitions.address(block), address);
    EXPECT_EQ(memory.read_u32(address), 0x12345678U);
    partitions.free(block);
    EXPECT_THROW(partitions.free(block), std::out_of_range);
    EXPECT_THROW(partitions.address(block), std::out_of_range);
    EXPECT_EQ(partitions.free_size(), 1024U);
    EXPECT_EQ(partitions.largest_free_size(), 1024U);
}

TEST(PartitionMemoryTest, AllocationPreservesAddressesAtTheEndOfTheGuestAddressSpace)
{
    std::uint32_t next_id{1};
    AddressArena arena(0xFFFFFC00, 0x100000000);
    PartitionMemory partitions(arena, next_id);
    const auto high = partitions.allocate({2, 1, 1});
    EXPECT_EQ(partitions.address(high), GuestAddress{0xFFFFFF00});
    EXPECT_EQ(partitions.free_size(), 768U);
}

} // namespace psp::detail
