#include "cpu/cpu.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

namespace psp
{

namespace
{

constexpr std::uint32_t program_base = 0x08800000;

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

} // namespace psp
