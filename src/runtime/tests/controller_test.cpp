#include "runtime/controller.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace psp::detail
{

TEST(ControllerTest, BlockingReadYieldsAndResumesOnlyAtGuestSampleWithExactLayout)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    Controller controller(memory, kernel);
    EXPECT_EQ(controller.set_sampling_cycle(0), 0U);
    EXPECT_EQ(controller.set_sampling_mode(1), 0U);
    controller.set_input({0xC010, 0, 255, 17, 200});
    const auto reader = kernel.current_thread_id();
    kernel.suspend_interrupts(); // Sampling and controller wakeups are independent of interrupt delivery.
    kernel.current_thread_state().registers[2] = 99;
    const auto saved = kernel.current_thread_state();
    memory.write_u32(GuestAddress{0x800}, 0xDEADBEEF);
    EXPECT_FALSE(controller.read_positive(GuestAddress{0x800}, 1));
    EXPECT_EQ(kernel.thread_status(reader).status.value(), 4U);
    EXPECT_EQ(kernel.current_thread_state(), saved);
    EXPECT_EQ(kernel.select_next_thread(16684), ThreadSelection::Idle);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x800}), 0xDEADBEEFU);
    controller.vblank();
    EXPECT_EQ(kernel.select_next_thread(16684), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_state().registers[2], 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x800}), 16684U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x804}), 0xC010U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x808}), 0xC811FF00U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x80C}), 0U);
    EXPECT_EQ(kernel.thread_status(reader).wait_type.value(), 0U);
    EXPECT_FALSE(controller.read_positive(GuestAddress{0x800}, 1)); // Already consumed.
}

TEST(ControllerTest, DigitalSamplesAreNeutralAndUnconsumedSamplesUseLatestGuestTimestamp)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    Controller controller(memory, kernel);
    controller.set_input({0x8, 0, 255, 10, 20});
    kernel.advance_time(std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 15);
    controller.vblank();
    kernel.advance_time(100);
    controller.vblank();
    EXPECT_EQ(controller.read_positive(GuestAddress{0x800}, 1), 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x800}), 114U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x804}), 8U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x808}), 0x80808080U);
    EXPECT_EQ(controller.set_sampling_mode(1), 0U);
    EXPECT_EQ(controller.set_sampling_mode(0), 1U);
}

TEST(ControllerTest, InvalidReadDoesNotConsumeSampleOrBlockCaller)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    Controller controller(memory, kernel);
    controller.vblank();
    EXPECT_THROW(controller.set_sampling_cycle(1), std::runtime_error);
    EXPECT_THROW(controller.set_sampling_mode(2), std::runtime_error);
    EXPECT_THROW(controller.read_positive(GuestAddress{0x800}, 2), std::runtime_error);
    EXPECT_THROW(controller.read_positive(GuestAddress{0x801}, 1), std::runtime_error);
    memory.write_u32(GuestAddress{0x3FFFC}, 0xDEADBEEF);
    EXPECT_THROW(controller.read_positive(GuestAddress{0x3FFFC}, 1), std::out_of_range);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x3FFFC}), 0xDEADBEEFU);
    EXPECT_EQ(kernel.thread_status(0).status.value(), 1U);
    EXPECT_EQ(controller.read_positive(GuestAddress{0x800}, 1), 1U);
}

TEST(ControllerTest, MultipleReadersReceiveDistinctSamplesInFifoOrder)
{
    Memory memory(GuestAddress{0}, 0x40000);
    Kernel kernel(memory, GuestAddress{0}, 0x40000, {}, GuestAddress{0});
    kernel.initialize(GuestAddress{0x400}, {});
    Controller controller(memory, kernel);
    const auto first = kernel.current_thread_id();
    const auto second = kernel.create_thread({GuestAddress{0x900}, 0x1000, 0x30, "second", 0});
    kernel.start_thread(second, GuestAddress{0}, 0);
    EXPECT_FALSE(controller.read_positive(GuestAddress{0x800}, 1));
    ASSERT_EQ(kernel.select_next_thread(100), ThreadSelection::Ready);
    ASSERT_EQ(kernel.current_thread_id(), second);
    EXPECT_FALSE(controller.read_positive(GuestAddress{0x820}, 1));
    ASSERT_EQ(kernel.select_next_thread(100), ThreadSelection::Idle);
    controller.set_input({0x10});
    controller.vblank();
    ASSERT_EQ(kernel.select_next_thread(100), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), first);
    EXPECT_EQ(kernel.thread_status(second).status.value(), 4U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x804}), 0x10U);
    kernel.exit_thread();
    ASSERT_EQ(kernel.select_next_thread(200), ThreadSelection::Idle);
    controller.set_input({0x20});
    controller.vblank();
    ASSERT_EQ(kernel.select_next_thread(200), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), second);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x820}), 200U);
    EXPECT_EQ(memory.read_u32(GuestAddress{0x824}), 0x20U);
}

} // namespace psp::detail
