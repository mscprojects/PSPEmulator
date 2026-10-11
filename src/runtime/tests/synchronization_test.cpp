#include "runtime/synchronization.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace psp::detail
{

TEST(SynchronizationTest, GivenWrongMutexOwner_WhenUnlocking_ThenGuestWorkAreaIsUnchanged)
{
    Memory memory(GuestAddress{0}, 0x40000);
    std::uint32_t next_id{1};
    Synchronization synchronization(memory, next_id);
    synchronization.create_mutex({GuestAddress{0x900}, GuestAddress{0x800}, 0x200, 0, GuestAddress{0}}, 7);
    synchronization.lock_mutex(GuestAddress{0x900}, 1, GuestAddress{0}, 7);
    synchronization.unlock_mutex(GuestAddress{0x900}, 1, 7);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x900}), 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x904}), 0U);

    synchronization.lock_mutex(GuestAddress{0x900}, 1, GuestAddress{0}, 7);
    const auto original = memory.read_bytes(GuestAddress{0x900}, 32);
    EXPECT_THROW(synchronization.unlock_mutex(GuestAddress{0x900}, 1, 9), std::runtime_error);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x900}, 32), original);
}

TEST(SynchronizationTest, EventFlagIdsAndCreationParametersAreValidated)
{
    Memory memory(GuestAddress{0}, 0x40000);
    std::uint32_t next_id{1};
    Synchronization synchronization(memory, next_id);
    memory.write_bytes(GuestAddress{0x800}, Payload{'G', 'U', 0});
    const auto id = synchronization.create_event_flag({GuestAddress{0x800}, 0x200, 3, GuestAddress{0}});
    synchronization.delete_event_flag(id);
    EXPECT_THROW(synchronization.delete_event_flag(id), std::runtime_error);
    EXPECT_THROW(synchronization.create_event_flag({GuestAddress{0x800}, 1, 3, GuestAddress{0}}), std::runtime_error);
    EXPECT_THROW(synchronization.create_event_flag({GuestAddress{0x800}, 0x200, 3, GuestAddress{4}}),
                 std::runtime_error);
    EXPECT_THROW(synchronization.create_event_flag({GuestAddress{0x40000}, 0x200, 3, GuestAddress{0}}),
                 std::out_of_range);
}

} // namespace psp::detail
