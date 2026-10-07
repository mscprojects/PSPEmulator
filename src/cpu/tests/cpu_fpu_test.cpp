#include "cpu/cpu.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace psp
{

namespace
{

constexpr std::uint32_t kProgramBase = 0x08800000;

class FpuTest : public testing::Test
{
protected:
    void instruction(std::uint32_t word)
    {
        memory.write_u32(state.program_counter, word);
        cpu.step(state);
    }

    Memory memory{GuestAddress{kProgramBase}, 256};
    Cpu cpu{memory};
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
};

struct ComparisonCase
{
    std::uint32_t left;
    std::uint32_t right;
    // Truth values for the eight quiet predicates, from C.F.S through C.ULE.S.
    std::uint8_t conditions;
};

} // namespace

TEST_F(FpuTest, TransfersEveryRegisterWithoutChangingPayloadBits)
{
    for (unsigned index = 0; index < 32; ++index)
    {
        for (const auto bits : {0x80000000U, 0x7FC12345U, 0x7F812345U, 0x00000001U, 0xFFFFFFFFU})
        {
            SCOPED_TRACE(index);
            SCOPED_TRACE(bits);
            state.program_counter = GuestAddress{kProgramBase};
            state.next_program_counter = GuestAddress{kProgramBase + 4};
            state.registers[8] = bits;
            instruction(0x44880000 | (index << 11)); // mtc1 $t0, $f[index]
            instruction(0x44090000 | (index << 11)); // mfc1 $t1, $f[index]
            EXPECT_EQ(state.registers[9], bits);
            instruction(0x46000006 | (index << 11) | (31U << 6)); // mov.s $f31, $f[index]
            EXPECT_EQ(state.floating_point_registers[31], bits);
            state.registers[10] = kProgramBase + 132;
            instruction(0xE55FFFFC); // swc1 $f31, -4($t2)
            EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 128}), bits);
            instruction(0xC540FFFC); // lwc1 $f0, -4($t2)
            EXPECT_EQ(state.floating_point_registers[0], bits);
        }
    }
}

TEST_F(FpuTest, LoadsFloatZeroAndRespectsIntegerZero)
{
    state.registers[8] = kProgramBase + 128;
    memory.write_u32(GuestAddress{kProgramBase + 128}, 0x7FC12345);
    instruction(0xC5000000); // lwc1 $f0, 0($t0)
    EXPECT_EQ(state.floating_point_registers[0], 0x7FC12345U);
    instruction(0x44000000); // mfc1 $zero, $f0
    EXPECT_EQ(state.registers[0], 0U);
    state.registers[0] = 99;
    instruction(0x44800000); // mtc1 $zero, $f0
    EXPECT_EQ(state.floating_point_registers[0], 0U);
}

TEST_F(FpuTest, ReadsStartupControlAndMasksWrites)
{
    instruction(0x4448F800); // cfc1 $t0, $31
    EXPECT_EQ(state.registers[8], 0x00000E00U);
    instruction(0x44480000); // cfc1 $t0, $0
    EXPECT_EQ(state.registers[8], 0x00003351U);
    state.registers[8] = 0xDEADBEEF;
    instruction(0x44C80000); // ctc1 $t0, $0 (read-only)
    instruction(0x44480000);
    EXPECT_EQ(state.registers[8], 0x00003351U);
    // Inputs and readbacks from the bundled hardware fcr.expected.
    constexpr std::array control_cases{
        std::array{0x00000003U, 0x00000003U}, std::array{0x0000007CU, 0x0000007CU},
        std::array{0x00000F80U, 0x00000F80U}, std::array{0x0001F000U, 0x0001F000U},
        std::array{0x01000000U, 0x01000000U}, std::array{0x00400000U, 0x00000000U},
        std::array{0x00200000U, 0x00000000U}, std::array{0x00800000U, 0x00800000U},
        std::array{0xFE000000U, 0x00000000U}, std::array{0x001C0000U, 0x00000000U},
    };
    for (const auto &entry : control_cases)
    {
        state.registers[8] = entry[0];
        instruction(0x44C8F800); // ctc1 $t0, $31
        instruction(0x4449F800); // cfc1 $t1, $31
        EXPECT_EQ(state.registers[9], entry[1]);
    }
    for (const unsigned index : {25U, 26U, 27U, 28U})
    {
        instruction(0x44480000 | (index << 11));
        EXPECT_EQ(state.registers[8], 0U);
    }
    instruction(0x4440F800); // cfc1 $zero, $31
    EXPECT_EQ(state.registers[0], 0U);
}

TEST_F(FpuTest, ComparesFiniteValuesZerosInfinitiesSubnormalsAndQuietNans)
{
    constexpr std::array cases{
        ComparisonCase{0x3F800000, 0x3F800000, 0xCC}, // equal
        ComparisonCase{0x3F800000, 0x40000000, 0xF0}, // less
        ComparisonCase{0x40000000, 0x3F800000, 0x00}, // greater
        ComparisonCase{0x80000000, 0x00000000, 0xCC}, // signed zeros
        ComparisonCase{0xC0000000, 0xBF800000, 0xF0}, // -2 < -1
        ComparisonCase{0xBF800000, 0xC0000000, 0x00}, // -1 > -2
        ComparisonCase{0xBF800000, 0x3F800000, 0xF0}, // -1 < 1
        ComparisonCase{0x7F800000, 0x7F800000, 0xCC}, // equal infinities
        ComparisonCase{0xFF800000, 0x7F800000, 0xF0}, // -inf < inf
        ComparisonCase{0x00000000, 0x00000001, 0xF0}, // positive subnormal
        ComparisonCase{0x80000001, 0x00000000, 0xF0}, // negative subnormal
        ComparisonCase{0x7FC12345, 0x3F800000, 0xAA}, // left NaN
        ComparisonCase{0x3F800000, 0xFFC12345, 0xAA}, // right NaN
        ComparisonCase{0x7FC12345, 0x7FC12345, 0xAA}, // NaN isn't equal to itself
    };
    for (const auto &entry : cases)
    {
        const unsigned predicates = entry.conditions == 0xAA ? 8 : 16;
        for (unsigned predicate = 0; predicate < predicates; ++predicate)
        {
            SCOPED_TRACE(entry.left);
            SCOPED_TRACE(entry.right);
            SCOPED_TRACE(predicate);
            state.program_counter = GuestAddress{kProgramBase};
            state.next_program_counter = GuestAddress{kProgramBase + 4};
            state.floating_point_registers[4] = entry.left;
            state.floating_point_registers[5] = entry.right;
            state.floating_point_control = 0x01000E03;
            instruction(0x46052030 | predicate); // c.[predicate].s $f4, $f5
            const bool expected = (entry.conditions & (1U << (predicate % 8))) != 0;
            EXPECT_EQ(state.floating_point_control, 0x01000E03U | (expected ? 0x00800000U : 0U));
        }
    }
}

TEST_F(FpuTest, BranchesExecuteOrSkipDelaySlotsForEveryConditionAndSelector)
{
    for (unsigned selector = 0; selector < 4; ++selector)
    {
        for (const bool condition : {false, true})
        {
            SCOPED_TRACE(selector);
            SCOPED_TRACE(condition);
            state = CpuState{.program_counter = GuestAddress{kProgramBase}};
            state.registers[8] = condition ? 0x00800000U : 0U;
            instruction(0x44C8F800);                                      // ctc1 $t0, $31
            memory.write_u32(GuestAddress{kProgramBase + 8}, 0x25290001); // delay: addiu $t1, $t1, 1
            instruction(0x45000003 | (selector << 16));                   // branch from +4 to +20
            const bool taken = condition == ((selector & 1U) != 0);
            const bool skipped = !taken && (selector & 2U) != 0;
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + (skipped ? 12 : 8));
            if (!skipped)
            {
                cpu.step(state);
            }
            EXPECT_EQ(state.registers[9], skipped ? 0U : 1U);
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + (taken ? 20 : 12));
        }
    }
}

TEST_F(FpuTest, SavesComparisonLatencyAndFloatingPointStateAcrossThreads)
{
    state.floating_point_registers[0] = 0x3F800000;
    state.floating_point_registers[1] = 0x3F800000;
    instruction(0x46010032); // c.eq.s $f0, $f1: true, branch still sees false
    auto snapshot = state;
    CpuState other{.program_counter = state.program_counter};
    other.registers[8] = 0;
    memory.write_u32(other.program_counter, 0x44C8F800); // other thread clears its own condition
    cpu.step(other);
    EXPECT_EQ(other.floating_point_control, 0U);
    instruction(0x45010003); // bc1t immediately after compare: not taken
    EXPECT_EQ(state.next_program_counter.value_of(), kProgramBase + 12);
    EXPECT_EQ(state.floating_point_control, 0x00800E00U);
    memory.write_u32(snapshot.program_counter, 0x45010003);
    cpu.step(snapshot);
    EXPECT_EQ(snapshot, state);
    instruction(0x4448F800); // cfc1 sees the new condition
    EXPECT_EQ(state.registers[8], 0x00800E00U);
    instruction(0x45010003); // now taken
    EXPECT_EQ(state.next_program_counter.value_of(), kProgramBase + 28);
    EXPECT_EQ(other.floating_point_registers[0], 0U);
}

TEST_F(FpuTest, MemoryFaultsPreserveFloatingPointStateAndPendingBranch)
{
    for (const auto word : {0xC5000000U, 0xE5000000U}) // lwc1 / swc1 $f0, 0($t0)
    {
        for (const auto address : {kProgramBase + 129, kProgramBase + 256})
        {
            state = CpuState{.program_counter = GuestAddress{kProgramBase}};
            state.registers[8] = address;
            state.floating_point_registers[0] = 0xDEADBEEF;
            instruction(0x10000007); // taken branch, target +32 pending
            memory.write_u32(state.program_counter, word);
            const auto before = state;
            EXPECT_THROW(cpu.step(state), std::exception);
            EXPECT_EQ(state, before);
            EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 128}), 0U);
            state.registers[8] = kProgramBase + 128;
            cpu.step(state);
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 32);
            memory.write_u32(GuestAddress{kProgramBase + 128}, 0);
        }
    }
}

TEST_F(FpuTest, RejectsInvalidOperationExceptionsWithoutCommittingState)
{
    for (const auto word : {0x4601003AU, 0x46010032U, 0x44C8F800U, 0x45040000U})
    {
        state = CpuState{.program_counter = GuestAddress{kProgramBase}};
        state.floating_point_registers[0] = word == 0x46010032 ? 0x7F812345 : 0x7FC12345;
        state.floating_point_registers[1] = 0x3F800000;
        state.registers[8] = 0x00020000;
        memory.write_u32(state.program_counter, word);
        const auto before = state;
        EXPECT_THROW(cpu.step(state), std::runtime_error);
        EXPECT_EQ(state, before);
    }
}

} // namespace psp
