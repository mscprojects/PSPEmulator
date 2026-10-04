#include "runtime/kernel.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace psp::detail
{

TEST(KernelTest, ThreadSelectionPreservesSeparateCpuStatesAndReturnStatus)
{
    Memory memory(GuestAddress{0}, 0x40000);
    GuestAllocator allocator(GuestAddress{0}, 0x40000, {});
    Kernel kernel(memory, allocator, GuestAddress{0x100});
    kernel.threads.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.threads.current_id();
    const auto second = kernel.threads.create({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    kernel.threads.start(second, GuestAddress{0}, 0);
    EXPECT_TRUE(kernel.threads.select_next());
    EXPECT_EQ(kernel.threads.current_id(), first);
    EXPECT_EQ(kernel.threads.status(second).status.value(), 2U); // ready

    const auto first_state = kernel.threads.current_state();
    kernel.threads.exit_current();
    EXPECT_TRUE(kernel.threads.select_next());
    EXPECT_EQ(kernel.threads.current_id(), second);
    EXPECT_EQ(kernel.threads.current_state().program_counter, GuestAddress{0x800});
    EXPECT_EQ(kernel.threads.current_state().next_program_counter, GuestAddress{0x804});
    EXPECT_EQ(kernel.threads.current_state().registers[28], 0x100U);
    EXPECT_EQ(kernel.threads.current_state().registers[31], first_state.registers[31]);
    EXPECT_EQ(kernel.threads.status(first).entry.value(), 0x400U);
    EXPECT_EQ(kernel.threads.status(first).status.value(), 16U); // stopped
    EXPECT_EQ(kernel.threads.status(0).name[0], 's');

    // Returning to the sentinel finishes only after the CPU has executed the
    // return's delay slot. The last thread's v0 supplies the execution status.
    auto &state = kernel.threads.current_state();
    state.program_counter = GuestAddress{state.registers[31]};
    state.registers[2] = 7;
    EXPECT_FALSE(kernel.threads.select_next());
    EXPECT_EQ(kernel.threads.exit_code(), 7);
}

TEST(KernelTest, ObjectIdentifiersAreSharedAcrossThreadPartitionAndSynchronizationOwners)
{
    Memory memory(GuestAddress{0}, 0x40000);
    GuestAllocator allocator(GuestAddress{0}, 0x40000, {});
    Kernel kernel(memory, allocator, GuestAddress{0});
    kernel.threads.initialize(GuestAddress{0x400}, {});
    const auto thread = kernel.threads.current_id();
    const auto block = kernel.allocate_partition({2, 0, 256});
    const auto semaphore = kernel.synchronization.create_semaphore({GuestAddress{0x800}, 0, 1, 1, GuestAddress{0}});
    kernel.synchronization.create_mutex({GuestAddress{0x900}, GuestAddress{0x800}, 0, 0, GuestAddress{0}}, thread);
    const auto mutex = memory.read_u32(GuestAddress{0x910});
    EXPECT_NE(thread, block);
    EXPECT_NE(block, semaphore);
    EXPECT_NE(semaphore, mutex);
    EXPECT_NE(thread, semaphore);
    EXPECT_NE(thread, mutex);
    EXPECT_NE(block, mutex);
    EXPECT_EQ(allocator.block_address(block), GuestAddress{256});
    EXPECT_THROW(allocator.block_address(semaphore), std::out_of_range);
}

TEST(KernelTest, WrongMutexOwnerCannotChangeGuestWorkArea)
{
    Memory memory(GuestAddress{0}, 0x40000);
    GuestAllocator allocator(GuestAddress{0}, 0x40000, {});
    Kernel kernel(memory, allocator, GuestAddress{0});
    kernel.threads.initialize(GuestAddress{0x400}, {});
    const auto thread = kernel.threads.current_id();
    const auto other = kernel.threads.create({GuestAddress{0x800}, 0x1000, 0x30, "other", 0});
    kernel.synchronization.create_mutex({GuestAddress{0x900}, GuestAddress{0x800}, 0x200, 0, GuestAddress{0}}, thread);
    kernel.synchronization.lock_mutex(GuestAddress{0x900}, 1, GuestAddress{0}, thread);
    const auto original = memory.read_bytes(GuestAddress{0x900}, 32);
    EXPECT_THROW(kernel.synchronization.unlock_mutex(GuestAddress{0x900}, 1, other), std::runtime_error);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x900}, 32), original);
    kernel.synchronization.unlock_mutex(GuestAddress{0x900}, 1, thread);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x900}), 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x904}), 0U);
}

TEST(GuestAllocatorTest, HeapAndStacksShareAnArenaAndFailedAllocationsPreserveCapacity)
{
    GuestAllocator allocator(GuestAddress{0x1000}, 1024, {});
    EXPECT_EQ(allocator.allocate(1, false), GuestAddress{0x1000});
    EXPECT_EQ(allocator.allocate(1, true), GuestAddress{0x1300});
    EXPECT_EQ(allocator.free_size(), 512U);
    EXPECT_THROW(allocator.allocate(513, false), std::runtime_error);
    EXPECT_EQ(allocator.free_size(), 512U);
    EXPECT_EQ(allocator.allocate(512, false), GuestAddress{0x1100});
    EXPECT_EQ(allocator.free_size(), 0U);
    EXPECT_THROW(allocator.allocate(1, true), std::runtime_error);
}

} // namespace psp::detail
