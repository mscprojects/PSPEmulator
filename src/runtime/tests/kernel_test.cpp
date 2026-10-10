#include "runtime/kernel.hpp"
#include "runtime/syscall_dispatcher.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>

namespace psp::detail
{

TEST(KernelTest, ThreadSelectionPreservesSeparateCpuStatesAndReturnStatus)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0x100});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    EXPECT_EQ(kernel.thread_status(second).status.value(), 16U); // created, not started
    kernel.start_thread(second, GuestAddress{0}, 0);
    EXPECT_THROW(kernel.start_thread(second, GuestAddress{0}, 0), std::runtime_error);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.thread_status(second).status.value(), 2U); // ready

    const auto first_state = kernel.current_thread_state();
    kernel.exit_thread();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
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
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
    EXPECT_EQ(kernel.exit_code(), 7);
}

TEST(KernelTest, DelayedThreadResumesAfterAnotherThreadExits)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
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
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), second);
    kernel.advance_time(500);
    kernel.current_thread_state().registers[2] = 7;
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.next_wakeup_time(), 2010U);
    kernel.advance_time(1500);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    auto resumed = saved;
    resumed.registers[2] = 0;
    EXPECT_EQ(kernel.current_thread_state(), resumed);
    EXPECT_EQ(kernel.thread_status(first).wait_type.value(), 0U);
    // Repeated selections must not enqueue the resumed thread twice.
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    kernel.current_thread_state().registers[2] = 9;
    kernel.exit_thread();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
    EXPECT_EQ(kernel.exit_code(), 9);
}

TEST(KernelTest, DelaySyscallDefersItsResultAndPreservesCommittedReturnAddress)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x800}, {});
    GuestIo io(memory);
    const std::array imports{PrxImportLibrary{
        .name = "ThreadManForUser", .version = 0, .attributes = 0, .functions = {{0xCEADEB47, GuestAddress{0x800}}}}};
    Display display(memory);
    Controller controller(memory, kernel);
    Ge ge(memory, kernel, 1000);
    SyscallDispatcher dispatcher(memory, kernel, io, display, controller, ge, imports);
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
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.next_wakeup_time(), 2000U);
    kernel.advance_time(2000);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state.program_counter, GuestAddress{0x900});
    EXPECT_EQ(state.registers[2], 0U);
}

TEST(KernelTest, EventWaitsEndWithTheirResultAndRejectReasonsWithDedicatedServices)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x800}, {});
    const auto main = kernel.current_thread_id();
    for (const auto reason :
         {Kernel::Wait::None, Kernel::Wait::SleepCallback, Kernel::Wait::Delay, Kernel::Wait::Vblank})
    {
        EXPECT_THROW(kernel.wait(reason), std::invalid_argument);
    }
    EXPECT_EQ(kernel.thread_status(main).status.value(), 1U);
    kernel.wait(Kernel::Wait::Controller);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_THROW(kernel.wake(main, Kernel::Wait::Ge, 0), std::logic_error);
    kernel.wake(main, Kernel::Wait::Controller, 7);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_state().registers[2], 7U);
}

TEST(KernelTest, WakeupsFollowDeadlinesAndEqualDeadlinesPreserveDelayOrder)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "second", 0});
    const auto third = kernel.create_thread({GuestAddress{0xC00}, 0x1000, 0x30, "third", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.start_thread(third, GuestAddress{0}, 0);
    kernel.delay_thread(100);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), second);
    kernel.delay_thread(20);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), third);
    kernel.delay_thread(20);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.next_wakeup_time(), 20U);
    kernel.advance_time(20);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.thread_status(third).status.value(), 2U);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 4U);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), third);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.next_wakeup_time(), 100U);
    kernel.advance_time(80);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.next_wakeup_time(), std::nullopt);
    kernel.exit_thread();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
}

TEST(KernelTest, ElapsedDelayDoesNotPreemptAnEqualPriorityRunningThread)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x20, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.delay_thread(50);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    kernel.advance_time(49);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 4U);
    kernel.advance_time(1);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(kernel.thread_status(first).status.value(), 2U);
    // Once ready, the thread is not added again on later CPU steps.
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 50U);
    kernel.exit_thread();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
}

TEST(KernelTest, ZeroDelayYieldsToReadyThreadsAndGameExitStopsPendingDelays)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto first = kernel.current_thread_id();
    kernel.delay_thread(0);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.system_time(), 0U);
    const auto second = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x20, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    kernel.delay_thread(0);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), second);
    kernel.delay_thread(1000);
    kernel.exit_game();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
    EXPECT_EQ(kernel.system_time(), 0U);
    EXPECT_EQ(kernel.exit_code(), 0);
}

TEST(KernelTest, GuestClockAndDelayDeadlinesRemainWideAndRejectOverflow)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    kernel.advance_time(0xFFFFFFFEULL);
    kernel.delay_thread(5);
    EXPECT_EQ(kernel.next_wakeup_time(), 0x100000003ULL);
    kernel.advance_time(5);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    kernel.advance_time(std::numeric_limits<std::uint64_t>::max() - kernel.system_time() - 1);
    EXPECT_THROW(kernel.delay_thread(2), std::overflow_error);
    EXPECT_EQ(kernel.thread_status(0).status.value(), 1U);
    EXPECT_THROW(kernel.advance_time(2), std::overflow_error);
    EXPECT_EQ(kernel.system_time(), std::numeric_limits<std::uint64_t>::max() - 1);
    kernel.delay_thread(1);
    EXPECT_EQ(kernel.next_wakeup_time(), std::numeric_limits<std::uint64_t>::max());
    kernel.advance_time(1);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
}

TEST(KernelTest, VblankInterruptsFollowLcdCadenceWithoutRoundingDrift)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    // 60000/1001 Hz gives edges at 16683 1/3, 33366 2/3, and 50050 us.
    EXPECT_FALSE(kernel.advance_time(16'683));
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(kernel.advance_time(1));
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(kernel.advance_time(16'682));
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(kernel.advance_time(1));
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(kernel.advance_time(16'682));
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(kernel.advance_time(1));
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(kernel.advance_time(1'001'000 - kernel.system_time() - 1));
    EXPECT_TRUE(kernel.deliver_pending_interrupt()); // Crossed multiple frames.
    EXPECT_TRUE(kernel.advance_time(1));
    EXPECT_TRUE(kernel.deliver_pending_interrupt()); // Exactly the 60th edge.
    EXPECT_FALSE(kernel.advance_time(0));
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
}

TEST(KernelTest, InterruptMakesStoreConditionalFailAndLoadLinkedCanRearmIt)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    Cpu cpu(memory);
    memory.write_u32(GuestAddress{0x400}, 0xC0850000); // ll $a1, 0($a0)
    memory.write_u32(GuestAddress{0x404}, 0xE0850000); // sc $a1, 0($a0)
    memory.write_u32(GuestAddress{0x408}, 0xC0850000);
    memory.write_u32(GuestAddress{0x40C}, 0xE0850000);
    memory.write_u32(GuestAddress{0x1000}, 0x11111111);
    auto &state = kernel.current_thread_state();
    state.registers[4] = 0x1000;
    cpu.step(state);
    state.registers[5] = 0xAAAAAAAA;
    const auto saved = state;
    kernel.advance_time(16'684);
    ASSERT_TRUE(kernel.deliver_pending_interrupt());
    auto resumed = saved;
    resumed.load_linked = false;
    EXPECT_EQ(state, resumed);
    cpu.step(state);
    EXPECT_EQ(state.registers[5], 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x1000}), 0x11111111U);
    cpu.step(state); // LL after interrupt return.
    state.registers[5] = 0xBBBBBBBB;
    cpu.step(state);
    EXPECT_EQ(state.registers[5], 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x1000}), 0xBBBBBBBBU);
}

TEST(KernelTest, InterruptPreservesBranchAndReturnDelaySlotsAndAllRegisters)
{
    for (const auto branch : {0x10000003U, 0x50000003U, 0x00200008U}) // BEQ, BEQL, JR
    {
        SCOPED_TRACE(branch);
        Memory memory(GuestAddress{0}, 0x40000);
        Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
        kernel.initialize(GuestAddress{0x400}, {});
        Cpu cpu(memory);
        memory.write_u32(GuestAddress{0x400}, branch);
        memory.write_u32(GuestAddress{0x404}, 0x26100001); // addiu $s0, $s0, 1 (delay slot)
        memory.write_u32(GuestAddress{0x410}, 0xE0850000); // sc $a1, 0($a0) (target)
        memory.write_u32(GuestAddress{0x1000}, 0x11111111);
        auto &state = kernel.current_thread_state();
        for (std::uint32_t index = 1; index < state.registers.size(); ++index)
        {
            state.registers[index] = index * 0x10101;
        }
        state.registers[1] = 0x410; // JR target.
        state.registers[4] = 0x1000;
        state.registers[5] = 0xAAAAAAAA;
        state.high_register = 0x12345678;
        state.low_register = 0x87654321;
        state.load_linked = true;
        cpu.step(state);
        ASSERT_EQ(state.program_counter, GuestAddress{0x404});
        ASSERT_EQ(state.next_program_counter, GuestAddress{0x410});
        auto resumed = state;
        resumed.load_linked = false;
        kernel.advance_time(16'684);
        ASSERT_TRUE(kernel.deliver_pending_interrupt());
        EXPECT_EQ(state, resumed);
        cpu.step(state);
        EXPECT_EQ(state.registers[16], resumed.registers[16] + 1);
        EXPECT_EQ(state.program_counter, GuestAddress{0x410});
        cpu.step(state);
        EXPECT_EQ(state.registers[5], 0U);
        EXPECT_EQ(memory.read_u32(GuestAddress{0x1000}), 0x11111111U);
    }
}

TEST(KernelTest, MaskedInterruptsStayPendingUntilTheOuterSuspendIsResumed)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    auto &state = kernel.current_thread_state();
    state.load_linked = true;
    const auto outer = kernel.suspend_interrupts();
    const auto inner = kernel.suspend_interrupts();
    EXPECT_EQ(outer, 1U);
    EXPECT_EQ(inner, 0U);
    kernel.advance_time(1'001'000); // Sixty masked edges coalesce into one pending event.
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(state.load_linked);
    kernel.resume_interrupts(inner);
    EXPECT_FALSE(kernel.interrupts_enabled());
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    kernel.resume_interrupts(outer);
    EXPECT_TRUE(kernel.interrupts_enabled());
    EXPECT_TRUE(state.load_linked); // Resume alone does not interrupt inside dispatch.
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(state.load_linked);
    state.load_linked = true;
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(state.load_linked);
    kernel.resume_interrupts(2); // Only the CPU interrupt-enable bit is restored.
    EXPECT_FALSE(kernel.interrupts_enabled());
}

TEST(KernelTest, IdleTimeSchedulesInterruptsWithoutChangingWaitingThreadState)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    auto &state = kernel.current_thread_state();
    state.load_linked = true;
    kernel.delay_thread(50'000);
    kernel.advance_time(16'684);
    EXPECT_FALSE(kernel.deliver_pending_interrupt()); // No running thread yet.
    EXPECT_TRUE(state.load_linked);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    kernel.advance_time(50'000 - 16'684);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(state.load_linked);
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    // An idle jump across edges must also generate an interrupt.
    state.load_linked = true;
    kernel.delay_thread(50'000);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_TRUE(kernel.advance_time(50'000));
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(state.load_linked);
}

TEST(KernelTest, ObjectIdentifiersAreSharedAcrossThreadsPartitionsAndSynchronization)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
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
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
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
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), other);
    EXPECT_THROW(kernel.unlock_mutex(GuestAddress{0x900}, 1), std::runtime_error);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x900}, 32), original);
}

TEST(KernelTest, HeapAndStacksShareAnArenaAndFailedAllocationsPreserveCapacity)
{
    Memory memory(GuestAddress{0x1000}, 1280);
    Kernel kernel(memory, {0x1000, 0x1500}, GuestAddress{0});
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

TEST(KernelTest, InvalidFreesAndAllocationSizesLeaveLiveBlocksIntact)
{
    Memory memory(GuestAddress{0x1000}, 1280);
    Kernel kernel(memory, {0x1000, 0x1500}, GuestAddress{0});
    const auto block = kernel.allocate_partition({2, 0, 257});
    const auto address = kernel.block_address(block);
    memory.write_u32(address, 0x12345678);
    EXPECT_THROW(kernel.free_partition(0), std::out_of_range);
    EXPECT_THROW(kernel.free_partition(0xDEADBEEF), std::out_of_range);
    for (const auto size : {0U, 513U, std::numeric_limits<std::uint32_t>::max()})
    {
        EXPECT_THROW(kernel.allocate_partition({2, 0, size}), std::runtime_error);
        EXPECT_THROW(kernel.allocate_partition({2, 1, size}), std::runtime_error);
    }
    EXPECT_EQ(kernel.free_memory_size(), 512U);
    EXPECT_EQ(kernel.largest_free_memory_size(), 512U);
    EXPECT_EQ(kernel.block_address(block), address);
    EXPECT_EQ(memory.read_u32(address), 0x12345678U);
    kernel.free_partition(block);
    EXPECT_THROW(kernel.free_partition(block), std::out_of_range);
    EXPECT_THROW(kernel.block_address(block), std::out_of_range);
    EXPECT_EQ(kernel.free_memory_size(), 1024U);
    EXPECT_EQ(kernel.largest_free_memory_size(), 1024U);
}

TEST(KernelTest, ThreadStackUsesAFreedPartitionRangeWithoutOverlappingLiveBlocks)
{
    Memory memory(GuestAddress{0x1000}, 2304);
    Kernel kernel(memory, {0x1000, 0x1900}, GuestAddress{0});
    const auto low = kernel.allocate_partition({2, 0, 512});
    const auto middle = kernel.allocate_partition({2, 0, 1024});
    const auto high = kernel.allocate_partition({2, 0, 512});
    const auto low_address = kernel.block_address(low);
    const auto high_address = kernel.block_address(high);
    memory.write_u32(low_address, 0x12345678);
    memory.write_u32(high_address, 0xABCDEF00);
    kernel.free_partition(middle);
    const auto thread = kernel.create_thread({GuestAddress{0x1000}, 512, 0x20, "worker", 0});
    const auto info = kernel.thread_status(thread);
    EXPECT_EQ(info.stack.value(), 0x1500U);
    EXPECT_EQ(info.stack_size.value(), 512U);
    EXPECT_EQ(kernel.free_memory_size(), 512U);
    EXPECT_THROW(kernel.free_partition(thread), std::out_of_range);
    const auto remainder = kernel.allocate_partition({2, 1, 512});
    EXPECT_EQ(kernel.block_address(remainder), GuestAddress{0x1300});
    EXPECT_EQ(kernel.free_memory_size(), 0U);
    EXPECT_EQ(memory.read_u32(low_address), 0x12345678U);
    EXPECT_EQ(memory.read_u32(high_address), 0xABCDEF00U);
}

TEST(KernelTest, AllocationPreservesAddressesAtTheEndOfTheGuestAddressSpace)
{
    Memory memory(GuestAddress{0xFFFFFB00}, 1280);
    Kernel kernel(memory, {0xFFFFFB00, 0x100000000}, GuestAddress{0});
    const auto high = kernel.allocate_partition({2, 1, 1});
    EXPECT_EQ(kernel.block_address(high), GuestAddress{0xFFFFFF00});
    EXPECT_EQ(kernel.free_memory_size(), 768U);
}

TEST(KernelTest, WaitingThreadKeepsItsStateAcrossVblanksUntilItsWakeup)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    auto &state = kernel.current_thread_state();
    state.load_linked = true;
    state.registers[2] = 0xDEADBEEF;
    kernel.delay_thread(50'050);
    EXPECT_EQ(kernel.next_vblank_time(), 16'684U);
    EXPECT_EQ(kernel.next_wakeup_time(), 50'050U);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_TRUE(kernel.advance_time(16'684));
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(state.registers[2], 0xDEADBEEFU);
    EXPECT_FALSE(kernel.deliver_pending_interrupt());
    EXPECT_TRUE(state.load_linked);
    EXPECT_EQ(kernel.next_vblank_time(), 33'367U);
    EXPECT_TRUE(kernel.advance_time(33'367 - 16'684));
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_TRUE(kernel.advance_time(50'050 - 33'367)); // The wakeup coincides with the third edge.
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state.registers[2], 0U);
    EXPECT_TRUE(kernel.deliver_pending_interrupt());
    EXPECT_FALSE(state.load_linked);
    EXPECT_EQ(kernel.next_vblank_time(), 66'734U);
    EXPECT_EQ(kernel.next_wakeup_time(), std::nullopt);
    kernel.advance_time(std::numeric_limits<std::uint64_t>::max() - kernel.system_time());
    EXPECT_THROW(kernel.next_vblank_time(), std::overflow_error);
}

TEST(KernelTest, HigherPriorityReadyThreadsPreemptAtBoundariesAndResumeSavedThreads)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto main = kernel.current_thread_id();
    const auto low = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x30, "low", 0});
    const auto middle = kernel.create_thread({GuestAddress{0x900}, 0x1000, 0x18, "middle", 0});
    const auto high = kernel.create_thread({GuestAddress{0xA00}, 0x1000, 0x10, "high", 0});
    for (const auto id : {low, middle, high})
    {
        kernel.start_thread(id, GuestAddress{0}, 0);
    }
    kernel.current_thread_state().registers[16] = 99;
    kernel.current_thread_state().load_linked = true;
    auto main_state = kernel.current_thread_state();
    main_state.load_linked = false;
    EXPECT_EQ(kernel.current_thread_id(), main); // StartThread does not switch inside dispatch.
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), high);
    kernel.delay_thread(50);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), middle);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    EXPECT_EQ(kernel.current_thread_state(), main_state);
    kernel.delay_thread(100);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), low);
    kernel.advance_time(50);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), high); // Expired delay preempts the lower-priority thread.
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), low);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.next_wakeup_time(), 100U);
    kernel.advance_time(50);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    kernel.exit_thread();
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Finished);
}

TEST(KernelTest, ExitCallbackRunsOnOwnerAndRestoresCpuStateWithoutEndingSleep)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0x100});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto main = kernel.current_thread_id();
    const auto owner = kernel.create_thread({GuestAddress{0x800}, 0x1000, 0x11, "callbacks", 0});
    kernel.start_thread(owner, GuestAddress{0}, 0);
    kernel.wait(Kernel::Wait::Controller);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), owner);
    const auto callback = kernel.create_callback(GuestAddress{0x900}, GuestAddress{0xC00});
    kernel.register_exit_callback(callback);
    auto &state = kernel.current_thread_state();
    state.program_counter = GuestAddress{0xA00}; // Continuation of SleepThreadCB.
    state.next_program_counter = GuestAddress{0xA04};
    state.registers[2] = 0xDEADBEEF;
    state.registers[16] = 44;
    state.high_register = 33;
    state.floating_point_registers[1] = 0x3F800000;
    state.load_linked = true;
    auto saved = state;
    saved.load_linked = false;
    kernel.sleep_thread_callbacks();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(kernel.thread_status(owner).wait_type.value(), 1U);
    kernel.request_exit();
    kernel.request_exit();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), owner);
    EXPECT_EQ(state.registers[4], 2U);
    EXPECT_EQ(state.registers[5], 0U);
    EXPECT_EQ(state.registers[6], 0xC00U);
    EXPECT_EQ(state.registers[28], 0x100U);
    EXPECT_LT(state.registers[29], saved.registers[29]);
    EXPECT_FALSE(state.load_linked);
    // Guest instructions write the callback ABI arguments and clobber saved state.
    constexpr std::array code{0xACC40000U, 0xACC50004U, 0xACC60008U, 0x00800011U, 0x24100063U,
                              0x44840800U, 0x24020000U, 0x03E00008U, 0U};
    for (std::size_t index = 0; index < code.size(); ++index)
    {
        memory.write_u32(GuestAddress{0x900 + static_cast<std::uint32_t>(index * 4)}, code[index]);
    }
    Cpu cpu(memory);
    for (std::size_t index = 0; index < code.size(); ++index)
    {
        EXPECT_FALSE(cpu.step(state));
        kernel.advance_time(1);
    }
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_EQ(state, saved);
    EXPECT_EQ(memory.read_u32(GuestAddress{0xC00}), 2U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0xC04}), 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0xC08}), 0xC00U);
    EXPECT_EQ(kernel.thread_status(owner).status.value(), 4U);
    kernel.wake(main, Kernel::Wait::Controller, 1);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    kernel.current_thread_state().load_linked = true;
    kernel.request_exit();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), owner);
    EXPECT_EQ(kernel.thread_status(main).status.value(), 2U);
    EXPECT_EQ(state.registers[4], 1U); // A fresh notification after the first was consumed.
    for (std::size_t index = 0; index < code.size(); ++index)
    {
        EXPECT_FALSE(cpu.step(state));
    }
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    EXPECT_FALSE(kernel.current_thread_state().load_linked);
}

TEST(KernelTest, EarlyExitRequestWaitsForCallbackEnabledSleepAndNonzeroReturnDeletesCallback)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    kernel.request_exit();
    const auto id = kernel.create_callback(GuestAddress{0x800}, GuestAddress{0});
    kernel.register_exit_callback(id);
    auto &state = kernel.current_thread_state();
    const auto original = state;
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state, original); // Registration does not invoke the function inline.
    kernel.delay_thread(10);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    kernel.advance_time(10);
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state.program_counter, original.program_counter); // Ordinary delay is not callback-enabled.
    kernel.sleep_thread_callbacks();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state.program_counter, GuestAddress{0x800});
    EXPECT_EQ(state.registers[4], 1U);
    EXPECT_THROW(kernel.sleep_thread_callbacks(), std::runtime_error);
    state.registers[2] = 1;
    state.program_counter = GuestAddress{state.registers[31]};
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_THROW(kernel.register_exit_callback(id), std::out_of_range);
    EXPECT_EQ(kernel.thread_status(0).status.value(), 4U);
}

TEST(KernelTest, CallbackValidationRejectsInvalidEntriesIdsAndInsufficientStack)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    EXPECT_THROW(kernel.create_callback(GuestAddress{0}, GuestAddress{0}), std::runtime_error);
    EXPECT_THROW(kernel.create_callback(GuestAddress{0x801}, GuestAddress{0}), std::runtime_error);
    EXPECT_THROW(kernel.create_callback(GuestAddress{0x40000}, GuestAddress{0}), std::out_of_range);
    EXPECT_THROW(kernel.register_exit_callback(999), std::out_of_range);
    const auto id = kernel.create_callback(GuestAddress{0x800}, GuestAddress{0});
    kernel.register_exit_callback(id);
    kernel.current_thread_state().registers[29] = kernel.thread_status(0).stack.value();
    const auto saved = kernel.current_thread_state();
    kernel.sleep_thread_callbacks();
    kernel.request_exit();
    EXPECT_THROW(kernel.select_next_thread(), std::runtime_error);
    EXPECT_EQ(kernel.current_thread_state(), saved);
}

TEST(KernelTest, GeInterruptPreservesIdleWaitAndCompletionsDuringItsExecution)
{
    // Both orders matter: completion during an interrupt and after its return.
    for (const auto kind : {0, 1, 2, 3})
    {
        for (const auto complete_during : {false, true})
        {
            SCOPED_TRACE(kind);
            SCOPED_TRACE(complete_during);
            Memory memory(GuestAddress{0}, 0x40000);
            Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
            kernel.initialize(GuestAddress{0x400}, {});
            const auto id = kernel.current_thread_id();
            auto saved = kernel.current_thread_state();
            if (kind == 0)
                kernel.wait(Kernel::Wait::Ge);
            if (kind == 1)
                kernel.wait(Kernel::Wait::Controller);
            if (kind == 2)
                kernel.delay_thread(10);
            if (kind == 3)
                kernel.wait_vblank();
            ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
            ASSERT_TRUE(kernel.enter_interrupt_callback(GuestAddress{0x800}, 7, GuestAddress{0x900}));
            EXPECT_FALSE(kernel.interrupts_enabled());
            EXPECT_FALSE(kernel.enter_interrupt_callback(GuestAddress{0x800}, 8, GuestAddress{0}));
            EXPECT_THROW(kernel.delay_thread(0), std::logic_error);
            EXPECT_THROW(kernel.wait(Kernel::Wait::Ge), std::logic_error);
            EXPECT_THROW(kernel.wait(Kernel::Wait::Controller), std::logic_error);
            EXPECT_THROW(kernel.sleep_thread_callbacks(), std::runtime_error);
            EXPECT_THROW(kernel.exit_thread(), std::runtime_error);
            const auto complete = [&]
            {
                if (kind == 0)
                    kernel.wake(id, Kernel::Wait::Ge, 0);
                if (kind == 1)
                    kernel.wake(id, Kernel::Wait::Controller, 1);
                if (kind == 2)
                    kernel.advance_time(10);
                if (kind == 3)
                    kernel.advance_time(16'684);
            };
            if (complete_during)
            {
                complete();
                ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
            }
            auto &state = kernel.current_thread_state();
            state.registers[2] = 99; // A GE interrupt's return value never deletes callbacks.
            state.program_counter = GuestAddress{state.registers[31]};
            if (!complete_during)
            {
                ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
                EXPECT_EQ(state, saved);
                complete();
            }
            ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
            saved.registers[2] = kind == 1 ? 1 : 0;
            EXPECT_EQ(state, saved);
            EXPECT_TRUE(kernel.interrupts_enabled());
            kernel.wait(Kernel::Wait::Ge); // No stale ready entry may resume this later wait.
            EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
        }
    }
}

TEST(KernelTest, GeInterruptDefersHigherPriorityThreadsUntilItsReturn)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto main = kernel.current_thread_id();
    ASSERT_TRUE(kernel.enter_interrupt_callback(GuestAddress{0x800}, 7, GuestAddress{0}));
    const auto high = kernel.create_thread({GuestAddress{0x900}, 0x1000, 0x10, "high", 0});
    kernel.start_thread(high, GuestAddress{0}, 0);
    // Even a guest re-enable cannot permit nested GE callbacks or thread preemption.
    kernel.resume_interrupts(1);
    EXPECT_FALSE(kernel.enter_interrupt_callback(GuestAddress{0x800}, 8, GuestAddress{0}));
    EXPECT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    auto &state = kernel.current_thread_state();
    state.program_counter = GuestAddress{state.registers[31]};
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), high);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
}

TEST(KernelTest, GeInterruptUsesAWaitingThreadAfterTheLastRunningThreadExits)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto main = kernel.current_thread_id();
    const auto worker = kernel.create_thread({GuestAddress{0x900}, 0x1000, 0x30, "worker", 0});
    kernel.start_thread(worker, GuestAddress{0}, 0);
    kernel.wait(Kernel::Wait::Ge);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), worker);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_TRUE(kernel.interrupt_callback_ready());
    ASSERT_TRUE(kernel.enter_interrupt_callback(GuestAddress{0x800}, 7, GuestAddress{0}));
    EXPECT_EQ(kernel.current_thread_id(), main);
    kernel.wake(main, Kernel::Wait::Ge, 0);
    auto &state = kernel.current_thread_state();
    state.program_counter = GuestAddress{state.registers[31]};
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    EXPECT_EQ(state.program_counter, GuestAddress{0x400});
    EXPECT_EQ(state.registers[2], 0U);
}

TEST(KernelTest, GeInterruptDefersWhileAnExitCallbackWaitsAndAllowsTimeToAdvance)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    const auto callback = kernel.create_callback(GuestAddress{0x800}, GuestAddress{0});
    kernel.register_exit_callback(callback);
    kernel.sleep_thread_callbacks();
    kernel.request_exit();
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    kernel.delay_thread(10);
    EXPECT_FALSE(kernel.interrupt_callback_ready());
    EXPECT_FALSE(kernel.enter_interrupt_callback(GuestAddress{0x900}, 7, GuestAddress{0}));
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    kernel.advance_time(10);
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
    auto &state = kernel.current_thread_state();
    state.registers[2] = 0;
    state.program_counter = GuestAddress{state.registers[31]};
    ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
    EXPECT_TRUE(kernel.interrupt_callback_ready());
    EXPECT_TRUE(kernel.enter_interrupt_callback(GuestAddress{0x900}, 7, GuestAddress{0}));
}

TEST(KernelTest, VblankWaitTargetsTheNextEdgeAndEventFlagIdsAreValidated)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, {0, 0x40000}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    for (const auto edge : {16'684U, 33'367U, 50'050U})
    {
        kernel.current_thread_state().registers[2] = 123;
        kernel.wait_vblank();
        EXPECT_EQ(kernel.thread_status(0).wait_type.value(), 12U);
        EXPECT_EQ(kernel.next_wakeup_time(), edge);
        ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Idle);
        EXPECT_TRUE(kernel.advance_time(edge - kernel.system_time()));
        ASSERT_EQ(kernel.select_next_thread(), ThreadSelection::Ready);
        EXPECT_EQ(kernel.current_thread_state().registers[2], 0U);
    }
    memory.write_bytes(GuestAddress{0x800}, Payload{'G', 'U', 0});
    const auto id = kernel.create_event_flag({GuestAddress{0x800}, 0x200, 3, GuestAddress{0}});
    kernel.delete_event_flag(id);
    EXPECT_THROW(kernel.delete_event_flag(id), std::runtime_error);
    EXPECT_THROW(kernel.create_event_flag({GuestAddress{0x800}, 1, 3, GuestAddress{0}}), std::runtime_error);
    EXPECT_THROW(kernel.create_event_flag({GuestAddress{0x800}, 0x200, 3, GuestAddress{4}}), std::runtime_error);
    EXPECT_THROW(kernel.create_event_flag({GuestAddress{0x40000}, 0x200, 3, GuestAddress{0}}), std::out_of_range);
}

} // namespace psp::detail
