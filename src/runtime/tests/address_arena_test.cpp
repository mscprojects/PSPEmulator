#include "runtime/address_arena.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace psp::detail
{

TEST(AddressArenaTest, BoundsAreAlignedInwardAndInvalidRangesAreRejected)
{
    AddressArena arena(0x1001, 0x14FF);
    EXPECT_EQ(arena.free_size(), 0x300U);
    EXPECT_EQ(arena.allocate(1, AllocationDirection::Low), (AddressRange{0x1100, 0x1200}));
    EXPECT_EQ(arena.allocate(1, AllocationDirection::High), (AddressRange{0x1300, 0x1400}));

    AddressArena empty(0x1001, 0x10FF);
    EXPECT_EQ(empty.free_size(), 0U);
    EXPECT_THROW(empty.allocate(1, AllocationDirection::Low), std::runtime_error);

    EXPECT_THROW(AddressArena(0x1200, 0x1100), std::invalid_argument);
    EXPECT_THROW(AddressArena(0xFFFFFC00, (std::uint64_t{1} << 32) + 1), std::invalid_argument);
}

TEST(AddressArenaTest, HolesAreReusedFromLowestAndHighestAddresses)
{
    AddressArena arena(0x1000, 0x1400);
    const auto first = arena.allocate(1, AllocationDirection::Low);
    arena.allocate(1, AllocationDirection::Low);
    const auto third = arena.allocate(1, AllocationDirection::Low);
    arena.allocate(1, AllocationDirection::Low);
    arena.free(first);
    arena.free(third);
    EXPECT_EQ(arena.free_size(), 512U);
    EXPECT_EQ(arena.largest_free_size(), 256U);
    EXPECT_THROW(arena.allocate(512, AllocationDirection::Low), std::runtime_error);
    EXPECT_EQ(arena.free_size(), 512U);

    EXPECT_EQ(arena.allocate(1, AllocationDirection::Low), first);
    EXPECT_EQ(arena.allocate(1, AllocationDirection::High), third);
    EXPECT_EQ(arena.free_size(), 0U);
    EXPECT_EQ(arena.largest_free_size(), 0U);
}

TEST(AddressArenaTest, AllocationSkipsHolesTooSmallForTheAlignedRequest)
{
    for (const auto direction : {AllocationDirection::Low, AllocationDirection::High})
    {
        SCOPED_TRACE(static_cast<int>(direction));
        AddressArena arena(0x1000, 0x1600);
        const auto low = arena.allocate(1, AllocationDirection::Low);
        arena.allocate(1, AllocationDirection::Low);
        const auto middle = arena.allocate(512, AllocationDirection::Low);
        arena.allocate(1, AllocationDirection::Low);
        const auto high = arena.allocate(1, AllocationDirection::Low);
        arena.free(low);
        arena.free(middle);
        arena.free(high);
        EXPECT_EQ(arena.allocate(257, direction), (AddressRange{0x1200, 0x1400}));
        EXPECT_EQ(arena.free_size(), 512U);
        EXPECT_EQ(arena.largest_free_size(), 256U);
    }
}

TEST(AddressArenaTest, FreeingRangesInAnyOrderRecoversOneContiguousRange)
{
    std::array<unsigned, 4> order{0, 1, 2, 3};
    do
    {
        SCOPED_TRACE(testing::PrintToString(order));
        AddressArena arena(0x1000, 0x1400);
        std::array<AddressRange, 4> ranges{};
        for (auto &range : ranges)
        {
            range = arena.allocate(1, AllocationDirection::Low);
        }
        for (std::size_t index = 0; index < order.size(); ++index)
        {
            arena.free(ranges[order[index]]);
            EXPECT_EQ(arena.free_size(), (index + 1) * 256);
        }
        EXPECT_EQ(arena.largest_free_size(), 1024U);
        const auto whole = arena.allocate(1024, AllocationDirection::Low);
        EXPECT_EQ(whole, (AddressRange{0x1000, 0x1400}));
        EXPECT_EQ(arena.free_size(), 0U);
        arena.free(whole);
        EXPECT_EQ(arena.free_size(), 1024U);
        EXPECT_EQ(arena.largest_free_size(), 1024U);
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(AddressArenaTest, InvalidSizesLeaveFreeSpaceUnchanged)
{
    AddressArena arena(0x1000, 0x1200);
    for (const auto direction : {AllocationDirection::Low, AllocationDirection::High})
    {
        for (const auto size : {0U, 513U, std::numeric_limits<std::uint32_t>::max()})
        {
            EXPECT_THROW(arena.allocate(size, direction), std::runtime_error);
        }
    }
    EXPECT_EQ(arena.free_size(), 512U);
    EXPECT_EQ(arena.largest_free_size(), 512U);
}

TEST(AddressArenaTest, RangesEndingAtTheTopOfTheAddressSpaceKeepTheirAddresses)
{
    constexpr std::uint64_t top = std::uint64_t{1} << 32;
    AddressArena arena(0xFFFFFC00, top);
    const auto high = arena.allocate(1, AllocationDirection::High);
    EXPECT_EQ(high, (AddressRange{0xFFFFFF00, top}));
    arena.free(high);
    EXPECT_EQ(arena.allocate(1024, AllocationDirection::Low), (AddressRange{0xFFFFFC00, top}));
    EXPECT_EQ(arena.free_size(), 0U);
}

} // namespace psp::detail
