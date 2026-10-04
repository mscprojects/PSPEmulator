#include "runtime/guest_structures.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace psp::detail
{

TEST(GuestStructuresTest, ThreadInfoUsesPspFieldOffsetsAndLittleEndianAddresses)
{
    Memory memory(GuestAddress{0}, 128);
    GuestThreadInfo info;
    info.name[0] = 't';
    info.status = static_cast<std::uint32_t>(ThreadStatus::Running);
    info.entry = 0x08801234;
    info.stack = 0x12345678;
    info.stack_size = 0x4000;
    info.exit_status = 0xFFFFFFFF;
    write_thread_info(memory, GuestAddress{0}, info);

    EXPECT_EQ(memory.read_u32(GuestAddress{0}), 104U);
    EXPECT_EQ(memory.read_c_string(GuestAddress{4}, 32), "t");
    EXPECT_EQ(memory.read_u32(GuestAddress{40}), 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{44}), 0x08801234U);
    EXPECT_EQ(memory.read_bytes(GuestAddress{48}, 4), (Payload{0x78, 0x56, 0x34, 0x12}));
    EXPECT_EQ(memory.read_u32(GuestAddress{52}), 0x4000U);
    EXPECT_EQ(memory.read_u32(GuestAddress{80}), 0xFFFFFFFFU);
}

TEST(GuestStructuresTest, InvalidOutputBufferDoesNotReceiveAPartialStructure)
{
    Memory memory(GuestAddress{0}, 103);
    const Payload original(103, 0xA5);
    memory.write_bytes(GuestAddress{0}, original);
    GuestThreadInfo info;
    info.stack = 0x08810000;
    EXPECT_THROW(write_thread_info(memory, GuestAddress{0}, info), std::out_of_range);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0}, original.size()), original);
}

TEST(GuestStructuresTest, MutexWorkAreaReadsGuestChangesAndPreservesReservedBytes)
{
    Memory memory(GuestAddress{0}, 32);
    memory.write_u32(GuestAddress{0}, 3);
    memory.write_u32(GuestAddress{4}, 7);
    memory.write_u32(GuestAddress{8}, 0x200);
    memory.write_u32(GuestAddress{16}, 9);
    memory.write_u32(GuestAddress{28}, 0x12345678);
    auto work_area = read_mutex_work_area(memory, GuestAddress{0});
    EXPECT_EQ(work_area.lock_count.value(), 3U);
    EXPECT_EQ(work_area.owner.value(), 7U);
    EXPECT_EQ(work_area.attributes.value(), 0x200U);
    EXPECT_EQ(work_area.id.value(), 9U);
    work_area.lock_count = 2;
    write_mutex_work_area(memory, GuestAddress{0}, work_area);
    EXPECT_EQ(memory.read_u32(GuestAddress{0}), 2U);
    EXPECT_EQ(memory.read_u32(GuestAddress{28}), 0x12345678U);
}

} // namespace psp::detail
