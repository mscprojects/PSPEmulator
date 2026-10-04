#include "runtime/kernel.hpp"
#include "runtime/syscall_dispatcher.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace psp::detail
{

TEST(KernelTest, ThreadSelectionPreservesSeparateCpuStatesAndReturnStatus)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0x100});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    EXPECT_EQ(kernel.thread_status(second).status.value(), 16U); // created, not started
    kernel.start_thread(second, GuestAddress{0}, 0);
    EXPECT_THROW(kernel.start_thread(second, GuestAddress{0}, 0), std::runtime_error);
    EXPECT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.thread_status(second).status.value(), 2U); // ready

    const auto first_state = kernel.current_thread_state();
    kernel.exit_thread();
    EXPECT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.current_thread_state().program_counter, GuestAddress{0x800});
    EXPECT_EQ(kernel.current_thread_state().next_program_counter, GuestAddress{0x804});
    EXPECT_EQ(kernel.current_thread_state().registers[28], 0x100U);
    EXPECT_EQ(kernel.current_thread_state().registers[31], first_state.registers[31]);
    EXPECT_EQ(kernel.thread_status(first).entry.value(), 0x400U);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 16U); // stopped
    EXPECT_THROW(kernel.start_thread(first, GuestAddress{0}, 0), std::runtime_error);
    EXPECT_EQ(kernel.thread_status(0).name[0], 's');

    // Returning to the sentinel finishes only after the CPU has executed the
    // return's delay slot. The last thread's v0 supplies the execution status.
    auto &state = kernel.current_thread_state();
    state.program_counter = GuestAddress{state.registers[31]};
    state.registers[2] = 7;
    EXPECT_FALSE(kernel.select_next_thread());
    EXPECT_EQ(kernel.exit_code(), 7);
}

TEST(KernelTest, DelayedThreadResumesAfterAnotherThreadExits)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    auto &state = kernel.current_thread_state();
    state.registers[2] = 0xDEADBEEF;
    state.registers[16] = 123;
    const auto saved = state;
    kernel.advance_time(10);
    kernel.delay_thread(2000);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 4U);
    EXPECT_EQ(kernel.thread_status(first).wait_type.value(), 2U);
    EXPECT_EQ(state, saved); // Dispatch has not supplied the delayed result yet.
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.system_time(), 10U); // A ready thread prevents an idle jump.
    kernel.advance_time(500);
    kernel.current_thread_state().registers[2] = 7;
    kernel.exit_thread();
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 2010U);
    auto resumed = saved;
    resumed.registers[2] = 0;
    EXPECT_EQ(kernel.current_thread_state(), resumed);
    EXPECT_EQ(kernel.thread_status(first).wait_type.value(), 0U);
    // Repeated selections must not enqueue the resumed thread twice.
    EXPECT_TRUE(kernel.select_next_thread());
    kernel.current_thread_state().registers[2] = 9;
    kernel.exit_thread();
    EXPECT_FALSE(kernel.select_next_thread());
    EXPECT_EQ(kernel.exit_code(), 9);
}

TEST(KernelTest, DelaySyscallDefersItsResultAndPreservesCommittedReturnAddress)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x800}, {});
    GuestIo io(memory);
    const std::array imports{PrxImportLibrary{
        .name = "ThreadManForUser", .version = 0, .attributes = 0, .functions = {{0xCEADEB47, GuestAddress{0x800}}}}};
    SyscallDispatcher dispatcher(memory, kernel, io, imports);
    Cpu cpu(memory);
    auto &state = kernel.current_thread_state();
    state.registers[31] = 0x900;
    state.registers[4] = 2000;
    state.registers[2] = 0xDEADBEEF;
    EXPECT_FALSE(cpu.step(state));        // Import stub JR.
    const auto syscall = cpu.step(state); // SYSCALL in its delay slot.
    ASSERT_TRUE(syscall);
    EXPECT_EQ(state.program_counter, GuestAddress{0x900});
    if (syscall)
    {
        dispatcher.handle(*syscall, state);
    }
    EXPECT_EQ(state.registers[2], 0xDEADBEEFU);
    EXPECT_EQ(kernel.thread_status(0).status.value(), 4U);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.system_time(), 2000U);
    EXPECT_EQ(state.program_counter, GuestAddress{0x900});
    EXPECT_EQ(state.registers[2], 0U);
}

TEST(KernelTest, IdleClockAdvancesToEarliestDeadlineAndPreservesEqualDeadlineOrder)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    const auto third = kernel.create_thread({GuestAddress{0xC00}, 0x1000, 0x30, "third", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.start_thread(third, GuestAddress{0}, 0);
    kernel.delay_thread(100);
    ASSERT_TRUE(kernel.select_next_thread());
    ASSERT_EQ(kernel.current_thread_id(), second);
    kernel.delay_thread(20);
    ASSERT_TRUE(kernel.select_next_thread());
    ASSERT_EQ(kernel.current_thread_id(), third);
    kernel.delay_thread(20);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.system_time(), 20U);
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.thread_status(third).status.value(), 2U);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 4U);
    kernel.exit_thread();
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), third);
    EXPECT_EQ(kernel.system_time(), 20U);
    kernel.exit_thread();
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 100U);
    kernel.exit_thread();
    EXPECT_FALSE(kernel.select_next_thread());
}

TEST(KernelTest, ElapsedDelayBecomesReadyWithoutPreemptingTheRunningThread)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.delay_thread(50);
    ASSERT_TRUE(kernel.select_next_thread());
    kernel.advance_time(49);
    EXPECT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.thread_status(first).status.value(), 4U);
    kernel.advance_time(1);
    EXPECT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 2U);
    // Once ready, the thread is not added again on later CPU steps.
    EXPECT_TRUE(kernel.select_next_thread());
    kernel.exit_thread();
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 50U);
    kernel.exit_thread();
    EXPECT_FALSE(kernel.select_next_thread());
}

TEST(KernelTest, ZeroDelayYieldsToReadyThreadsAndGameExitStopsPendingDelays)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    kernel.delay_thread(0);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 0U);
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.delay_thread(0);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.current_thread_id(), second);
    kernel.delay_thread(1000);
    kernel.exit_game();
    EXPECT_FALSE(kernel.select_next_thread());
    EXPECT_EQ(kernel.system_time(), 0U);
    EXPECT_EQ(kernel.exit_code(), 0);
}

TEST(KernelTest, GuestClockAndDelayDeadlinesRemainWideAndRejectOverflow)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    kernel.advance_time(0xFFFFFFFEULL);
    kernel.delay_thread(5);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.system_time(), 0x100000003ULL);
    kernel.advance_time(std::numeric_limits<std::uint64_t>::max() - kernel.system_time() - 1);
    EXPECT_THROW(kernel.delay_thread(2), std::overflow_error);
    EXPECT_EQ(kernel.thread_status(0).status.value(), 1U);
    EXPECT_THROW(kernel.advance_time(2), std::overflow_error);
    EXPECT_EQ(kernel.system_time(), std::numeric_limits<std::uint64_t>::max() - 1);
    kernel.delay_thread(1);
    ASSERT_TRUE(kernel.select_next_thread());
    EXPECT_EQ(kernel.system_time(), std::numeric_limits<std::uint64_t>::max());
}

TEST(KernelTest, ObjectIdentifiersAreSharedAcrossThreadsPartitionsAndSynchronization)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto thread = kernel.current_thread_id();
    const auto block = kernel.allocate_partition({2, 0, 256});
    const auto semaphore = kernel.create_semaphore({GuestAddress{0x800}, 0, 1, 1, GuestAddress{0}});
    kernel.create_mutex({GuestAddress{0x900}, GuestAddress{0x800}, 0, 0, GuestAddress{0}});
    const auto mutex = memory.read_u32(GuestAddress{0x910});
    EXPECT_NE(thread, block);
    EXPECT_NE(block, semaphore);
    EXPECT_NE(semaphore, mutex);
    EXPECT_NE(thread, semaphore);
    EXPECT_NE(thread, mutex);
    EXPECT_NE(block, mutex);
    EXPECT_EQ(kernel.block_address(block), GuestAddress{256});
    EXPECT_THROW(kernel.block_address(semaphore), std::out_of_range);
}

TEST(KernelTest, WrongMutexOwnerCannotChangeGuestWorkArea)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto other = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "other", 0});
    kernel.create_mutex({GuestAddress{0x900}, GuestAddress{0x800}, 0x200, 0, GuestAddress{0}});
    kernel.lock_mutex(GuestAddress{0x900}, 1, GuestAddress{0});
    kernel.unlock_mutex(GuestAddress{0x900}, 1);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x900}), 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x904}), 0U);

    kernel.lock_mutex(GuestAddress{0x900}, 1, GuestAddress{0});
    const auto original = memory.read_bytes(GuestAddress{0x900}, 32);
    kernel.start_thread(other, GuestAddress{0}, 0);
    kernel.exit_thread();
    ASSERT_TRUE(kernel.select_next_thread());
    ASSERT_EQ(kernel.current_thread_id(), other);
    EXPECT_THROW(kernel.unlock_mutex(GuestAddress{0x900}, 1), std::runtime_error);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x900}, 32), original);
}

TEST(KernelTest, HeapAndStacksShareAnArenaAndFailedAllocationsPreserveCapacity)
{
    Memory memory(GuestAddress{0x1000}, 1280);
    Kernel kernel(memory, GuestAddress{0x1000}, 1280, {}, GuestAddress{0});
    // The kernel's return sentinel reserves the first 256 bytes.
    const auto low = kernel.allocate_partition({2, 0, 1});
    const auto high = kernel.allocate_partition({2, 1, 1});
    EXPECT_EQ(kernel.block_address(low), GuestAddress{0x1100});
    EXPECT_EQ(kernel.block_address(high), GuestAddress{0x1400});
    EXPECT_EQ(kernel.free_memory_size(), 512U);
    EXPECT_THROW(kernel.allocate_partition({2, 0, 513}), std::runtime_error);
    EXPECT_EQ(kernel.free_memory_size(), 512U);
    const auto remaining = kernel.allocate_partition({2, 0, 512});
    EXPECT_EQ(kernel.block_address(remaining), GuestAddress{0x1200});
    EXPECT_EQ(kernel.free_memory_size(), 0U);
    EXPECT_THROW(kernel.allocate_partition({2, 1, 1}), std::runtime_error);
}

} // namespace psp::detail
