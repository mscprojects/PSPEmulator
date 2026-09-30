#include "cpu/cpu.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

namespace psp
{

namespace
{

constexpr std::uint32_t program_base = 0x08800000;

struct BranchCase
{
    const char *name;
    std::uint32_t instruction;
    bool likely;
    bool links;
    // Expected conditions for rs = -1, 0, 1, INT32_MIN; rt = zero.
    std::array<bool, 4> taken;
};

void load_program(Memory &memory, std::initializer_list<std::uint32_t> instructions)
{
    auto address = program_base;
    for (const auto instruction : instructions)
    {
        memory.write_u32(GuestAddress{address}, instruction);
        address += 4;
    }
}

void step_n(Cpu &cpu, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        cpu.step();
    }
}

} // namespace

TEST(CpuTest, ExecutesArithmeticAndLogicalInstructions)
{
    Memory memory(GuestAddress{program_base}, 64);
    load_program(memory, {
                             0x3C081234, // lui   $t0, 0x1234
                             0x35085678, // ori   $t0, $t0, 0x5678
                             0x2409FFFF, // addiu $t1, $zero, -1
                             0x01095021, // addu  $t2, $t0, $t1
                             0x01095823, // subu  $t3, $t0, $t1
                             0x01096024, // and   $t4, $t0, $t1
                             0x01096825, // or    $t5, $t0, $t1
                             0x01097026, // xor   $t6, $t0, $t1
                             0x01097827, // nor   $t7, $t0, $t1
                         });
    Cpu cpu(memory, GuestAddress{program_base});

    step_n(cpu, 9);

    EXPECT_EQ(cpu.register_value(8), 0x12345678U);
    EXPECT_EQ(cpu.register_value(9), 0xFFFFFFFFU);
    EXPECT_EQ(cpu.register_value(10), 0x12345677U);
    EXPECT_EQ(cpu.register_value(11), 0x12345679U);
    EXPECT_EQ(cpu.register_value(12), 0x12345678U);
    EXPECT_EQ(cpu.register_value(13), 0xFFFFFFFFU);
    EXPECT_EQ(cpu.register_value(14), 0xEDCBA987U);
    EXPECT_EQ(cpu.register_value(15), 0U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 36);
}

TEST(CpuTest, ExecutesComparisonsAndShifts)
{
    Memory memory(GuestAddress{program_base}, 64);
    load_program(memory, {
                             0x2408FFFF, // addiu $t0, $zero, -1
                             0x24090001, // addiu $t1, $zero, 1
                             0x0109502A, // slt   $t2, $t0, $t1
                             0x0109582B, // sltu  $t3, $t0, $t1
                             0x290C0000, // slti  $t4, $t0, 0
                             0x2D0DFFFF, // sltiu $t5, $t0, -1
                             0x2C0EFFFF, // sltiu $t6, $zero, -1
                             0x00087880, // sll   $t7, $t0, 2
                             0x00088082, // srl   $s0, $t0, 2
                             0x00088883, // sra   $s1, $t0, 2
                             0x3112FFF0, // andi  $s2, $t0, 0xfff0
                             0x3913FFFF, // xori  $s3, $t0, 0xffff
                         });
    Cpu cpu(memory, GuestAddress{program_base});

    step_n(cpu, 12);

    EXPECT_EQ(cpu.register_value(10), 1U);
    EXPECT_EQ(cpu.register_value(11), 0U);
    EXPECT_EQ(cpu.register_value(12), 1U);
    EXPECT_EQ(cpu.register_value(13), 0U);
    EXPECT_EQ(cpu.register_value(14), 1U);
    EXPECT_EQ(cpu.register_value(15), 0xFFFFFFFCU);
    EXPECT_EQ(cpu.register_value(16), 0x3FFFFFFFU);
    EXPECT_EQ(cpu.register_value(17), 0xFFFFFFFFU);
    EXPECT_EQ(cpu.register_value(18), 0xFFF0U);
    EXPECT_EQ(cpu.register_value(19), 0xFFFF0000U);
}

TEST(CpuTest, KeepsZeroRegisterConstant)
{
    Memory memory(GuestAddress{program_base}, 16);
    load_program(memory, {
                             0x3C00FFFF, // lui   $zero, 0xffff
                             0x24000001, // addiu $zero, $zero, 1
                             0x00000000, // nop
                         });
    Cpu cpu(memory, GuestAddress{program_base});

    step_n(cpu, 3);

    EXPECT_EQ(cpu.register_value(0), 0U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
}

TEST(CpuTest, LoadsAndStoresWordsThroughMemory)
{
    Memory memory(GuestAddress{program_base}, 64);
    load_program(memory, {
                             0x3C080880, // lui   $t0, 0x0880
                             0x24091234, // addiu $t1, $zero, 0x1234
                             0xAD090020, // sw    $t1, 32($t0)
                             0x8D0A0020, // lw    $t2, 32($t0)
                         });
    Cpu cpu(memory, GuestAddress{program_base});

    step_n(cpu, 4);

    EXPECT_EQ(memory.read_u32(GuestAddress{program_base + 32}), 0x1234U);
    EXPECT_EQ(cpu.register_value(10), 0x1234U);
}

TEST(CpuTest, RejectsUnsupportedAndMisalignedInstructionsWithoutAdvancing)
{
    Memory memory(GuestAddress{program_base}, 16);
    load_program(memory, {0xFFFFFFFF});
    Cpu cpu(memory, GuestAddress{program_base});

    EXPECT_THROW(cpu.step(), std::runtime_error);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base);

    memory.write_u32(GuestAddress{program_base}, 0x8C080001); // lw $t0, 1($zero)
    EXPECT_THROW(cpu.step(), std::invalid_argument);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base);
    EXPECT_THROW(Cpu(memory, GuestAddress{program_base + 1}), std::invalid_argument);
}

TEST(CpuTest, ExecutesEveryIntegerBranchVariant)
{
    const BranchCase cases[] = {
        {"beq", 0x11000002, false, false, {false, true, false, false}},
        {"bne", 0x15000002, false, false, {true, false, true, true}},
        {"blez", 0x19000002, false, false, {true, true, false, true}},
        {"bgtz", 0x1D000002, false, false, {false, false, true, false}},
        {"beql", 0x51000002, true, false, {false, true, false, false}},
        {"bnel", 0x55000002, true, false, {true, false, true, true}},
        {"blezl", 0x59000002, true, false, {true, true, false, true}},
        {"bgtzl", 0x5D000002, true, false, {false, false, true, false}},
        {"bltz", 0x05000002, false, false, {true, false, false, true}},
        {"bgez", 0x05010002, false, false, {false, true, true, false}},
        {"bltzl", 0x05020002, true, false, {true, false, false, true}},
        {"bgezl", 0x05030002, true, false, {false, true, true, false}},
        {"bltzal", 0x05100002, false, true, {true, false, false, true}},
        {"bgezal", 0x05110002, false, true, {false, true, true, false}},
        {"bltzall", 0x05120002, true, true, {true, false, false, true}},
        {"bgezall", 0x05130002, true, true, {false, true, true, false}},
    };
    const std::uint32_t values[] = {0xFFFFFFFF, 0, 1, 0x80000000};
    for (const auto &test : cases)
    {
        for (std::size_t index = 0; index < std::size(values); ++index)
        {
            SCOPED_TRACE(test.name);
            SCOPED_TRACE(values[index]);
            Memory memory(GuestAddress{program_base}, 64);
            load_program(memory, {
                                     0x3C080000 | (values[index] >> 16),    // lui $t0, upper half
                                     0x35080000 | (values[index] & 0xFFFF), // ori $t0, $t0, lower half
                                     test.instruction,                      // branch to base + 20
                                     0x27EA0001,                            // addiu $t2, $ra, 1 (delay slot)
                                     0x240B0001,                            // addiu $t3, $zero, 1 (fallthrough)
                                     0x240C0001,                            // addiu $t4, $zero, 1 (target)
                                 });
            Cpu cpu(memory, GuestAddress{program_base});
            step_n(cpu, 3);

            const bool skipped = test.likely && !test.taken[index];
            EXPECT_EQ(cpu.program_counter().value_of(), program_base + (skipped ? 16 : 12));
            EXPECT_EQ(cpu.register_value(31), test.links ? program_base + 16 : 0U);
            EXPECT_EQ(cpu.register_value(10), 0U);
            if (!skipped)
            {
                cpu.step();
                EXPECT_EQ(cpu.register_value(10), test.links ? program_base + 17 : 1U);
            }
            EXPECT_EQ(cpu.program_counter().value_of(), program_base + (test.taken[index] ? 20 : 16));
            cpu.step();
            EXPECT_EQ(cpu.register_value(11), test.taken[index] ? 0U : 1U);
            EXPECT_EQ(cpu.register_value(12), test.taken[index] ? 1U : 0U);
            EXPECT_EQ(cpu.program_counter().value_of(), program_base + (test.taken[index] ? 24 : 20));
        }
    }
}

TEST(CpuTest, BranchConditionIsEvaluatedBeforeDelaySlot)
{
    Memory memory(GuestAddress{program_base}, 32);
    load_program(memory, {
                             0x11000002, // beq $t0, $zero, base + 12
                             0x24080001, // addiu $t0, $zero, 1
                             0x24090001, // addiu $t1, $zero, 1 (skipped)
                             0x00000000,
                         });
    Cpu cpu(memory, GuestAddress{program_base});
    step_n(cpu, 2);
    EXPECT_EQ(cpu.register_value(8), 1U);
    EXPECT_EQ(cpu.register_value(9), 0U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
}

TEST(CpuTest, ExecutesBackwardLoopWithDelaySlots)
{
    Memory memory(GuestAddress{program_base}, 32);
    load_program(memory, {
                             0x24080003, // addiu $t0, $zero, 3
                             0x2508FFFF, // addiu $t0, $t0, -1
                             0x1500FFFE, // bne $t0, $zero, base + 4
                             0x25290001, // addiu $t1, $t1, 1 (runs on all three iterations)
                         });
    Cpu cpu(memory, GuestAddress{program_base});
    step_n(cpu, 10);
    EXPECT_EQ(cpu.register_value(8), 0U);
    EXPECT_EQ(cpu.register_value(9), 3U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 16);
}

TEST(CpuTest, CallsFunctionAndReturnsAfterBothDelaySlots)
{
    Memory memory(GuestAddress{program_base}, 32);
    load_program(memory, {
                             0x0E200004, // jal base + 16
                             0x24040007, // addiu $a0, $zero, 7 (call delay slot)
                             0x244B0000, // addiu $t3, $v0, 0 (return address)
                             0x00000000,
                             0x24820001, // addiu $v0, $a0, 1
                             0x03E00008, // jr $ra
                             0x24420002, // addiu $v0, $v0, 2 (return delay slot)
                         });
    Cpu cpu(memory, GuestAddress{program_base});
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 4);
    EXPECT_EQ(cpu.register_value(31), program_base + 8);
    EXPECT_EQ(cpu.register_value(4), 0U);
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 16);
    EXPECT_EQ(cpu.register_value(4), 7U);
    step_n(cpu, 2);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 24);
    EXPECT_EQ(cpu.register_value(2), 8U);
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 8);
    EXPECT_EQ(cpu.register_value(2), 10U);
    cpu.step();
    EXPECT_EQ(cpu.register_value(11), 10U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
}

TEST(CpuTest, JalrCapturesTargetBeforeWritingAnyLinkRegister)
{
    // Link to a different register, to the source register, or to $zero.
    for (const std::uint32_t destination : {9U, 8U, 0U})
    {
        SCOPED_TRACE(destination);
        Memory memory(GuestAddress{program_base}, 32);
        load_program(memory, {
                                 0x3C080880,                       // lui $t0, 0x0880
                                 0x35080018,                       // ori $t0, $t0, 24
                                 0x01000009 | (destination << 11), // jalr rd, $t0
                                 0x250A0000,                       // addiu $t2, $t0, 0 (delay slot reads updated link)
                                 0xFFFFFFFF,                       // must be skipped
                                 0xFFFFFFFF,
                                 0x240B0007, // addiu $t3, $zero, 7
                             });
        Cpu cpu(memory, GuestAddress{program_base});
        step_n(cpu, 3);
        EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
        EXPECT_EQ(cpu.register_value(destination), destination == 0 ? 0U : program_base + 16);
        EXPECT_EQ(cpu.register_value(31), 0U);
        cpu.step();
        EXPECT_EQ(cpu.register_value(10), program_base + (destination == 8 ? 16 : 24));
        EXPECT_EQ(cpu.program_counter().value_of(), program_base + 24);
        cpu.step();
        EXPECT_EQ(cpu.register_value(11), 7U);
    }
}

TEST(CpuTest, JumpUsesUpperBitsOfPcPlusFour)
{
    // Cross a 256 MiB boundary: PC and PC + 4 have different upper bits.
    Memory memory(GuestAddress{0x0FFFFFFC}, 12);
    memory.write_u32(GuestAddress{0x0FFFFFFC}, 0x08000001); // j 0x10000004
    memory.write_u32(GuestAddress{0x10000000}, 0x24080001); // delay slot
    memory.write_u32(GuestAddress{0x10000004}, 0x24090002);
    Cpu cpu(memory, GuestAddress{0x0FFFFFFC});
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), 0x10000000U);
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), 0x10000004U);
    EXPECT_EQ(cpu.register_value(8), 1U);
    EXPECT_EQ(cpu.register_value(31), 0U);
    cpu.step();
    EXPECT_EQ(cpu.register_value(9), 2U);
}

TEST(CpuTest, UntakenLikelyBranchDoesNotExecuteInvalidDelaySlot)
{
    Memory memory(GuestAddress{program_base}, 16);
    load_program(memory, {
                             0x54000002, // bnel $zero, $zero, base + 12 (untaken)
                             0xFFFFFFFF, // invalid delay slot must not be decoded
                             0x24080001,
                         });
    Cpu cpu(memory, GuestAddress{program_base});
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 8);
    cpu.step();
    EXPECT_EQ(cpu.register_value(8), 1U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
}

TEST(CpuTest, FailedDelaySlotPreservesPendingBranchForRetry)
{
    Memory memory(GuestAddress{program_base}, 20);
    load_program(memory, {0x10000002, 0xFFFFFFFF, 0xFFFFFFFF, 0x24080007});
    Cpu cpu(memory, GuestAddress{program_base});
    cpu.step();
    EXPECT_THROW(cpu.step(), std::runtime_error);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 4);
    memory.write_u32(GuestAddress{program_base + 4}, 0x8C080001); // misaligned lw
    EXPECT_THROW(cpu.step(), std::invalid_argument);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 4);
    memory.write_u32(GuestAddress{program_base + 4}, 0x00000000); // repair delay slot
    cpu.step();
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 12);
    cpu.step();
    EXPECT_EQ(cpu.register_value(8), 7U);
}

TEST(CpuTest, MisalignedRegisterJumpFaultsAfterExecutingDelaySlot)
{
    Memory memory(GuestAddress{program_base}, 24);
    load_program(memory, {
                             0x3C080880,
                             0x35080001, // $t0 = base + 1
                             0x01000008, // jr $t0
                             0x24090007,
                         });
    Cpu cpu(memory, GuestAddress{program_base});
    step_n(cpu, 4);
    EXPECT_EQ(cpu.register_value(9), 7U);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 1);
    EXPECT_THROW(cpu.step(), std::invalid_argument);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 1);
}

TEST(CpuTest, RejectsUnknownRegimmWithoutChangingControlFlow)
{
    Memory memory(GuestAddress{program_base}, 16);
    load_program(memory, {0x041F0001}); // unsupported REGIMM operation
    Cpu cpu(memory, GuestAddress{program_base});
    EXPECT_THROW(cpu.step(), std::runtime_error);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base);
    EXPECT_EQ(cpu.register_value(31), 0U);
    memory.write_u32(GuestAddress{program_base}, 0x00000000);
    step_n(cpu, 2);
    EXPECT_EQ(cpu.program_counter().value_of(), program_base + 8);
}

} // namespace psp
