#include "cpu/cpu.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cfenv>
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
    void SetUp() override
    {
        ASSERT_EQ(std::fegetenv(&saved_environment), 0);
        saved_errno = errno;
    }

    void TearDown() override
    {
        std::fesetenv(&saved_environment);
        errno = saved_errno;
    }

    // Opcode fields, operand payloads, and FCR31 are separately encoded guest words.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void arithmetic(std::uint32_t function, std::uint32_t left, std::uint32_t right, std::uint32_t control = 0)
    {
        state = CpuState{.program_counter = GuestAddress{kProgramBase}};
        state.floating_point_registers[1] = left;
        state.floating_point_registers[2] = right;
        state.floating_point_control = control;
        const auto target = function <= 3 ? 2U << 16 : 0U;
        const auto format = function == 0x20 ? 0x46800000U : 0x46000000U;
        instruction(format | target | (1U << 11) | (3U << 6) | function);
    }

    void instruction(std::uint32_t word)
    {
        memory.write_u32(state.program_counter, word);
        cpu.step(state);
    }

    std::fenv_t saved_environment{};
    int saved_errno{};
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
        ComparisonCase{0x7FBFFFFF, 0x7FBFFFFF, 0xAA}, // libc NAN in the bundled fpu.prx
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
    for (const auto word : {0x4601003AU, 0x44C8F800U, 0x45040000U})
    {
        state = CpuState{.program_counter = GuestAddress{kProgramBase}};
        state.floating_point_registers[0] = 0x7FC12345;
        state.floating_point_registers[1] = 0x3F800000;
        state.registers[8] = 0x00020000;
        memory.write_u32(state.program_counter, word);
        const auto before = state;
        EXPECT_THROW(cpu.step(state), std::runtime_error);
        EXPECT_EQ(state, before);
    }
}

TEST_F(FpuTest, ArithmeticAndConversionsFollowGuestRoundingWithFixedModeOverrides)
{
    constexpr std::array positive_sum{0x3F800000U, 0x3F800000U, 0x3F800001U, 0x3F800000U};
    constexpr std::array negative_sum{0xBF800000U, 0xBF800000U, 0xBF800000U, 0xBF800001U};
    constexpr std::array quotient{0x3EAAAAABU, 0x3EAAAAAAU, 0x3EAAAAABU, 0x3EAAAAAAU};
    constexpr std::array square_root{0x3FB504F3U, 0x3FB504F3U, 0x3FB504F4U, 0x3FB504F3U};
    constexpr std::array integer_to_float{0x4B800000U, 0x4B800000U, 0x4B800001U, 0x4B800000U};
    constexpr std::array converted{2U, 2U, 3U, 2U};
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        SCOPED_TRACE(mode);
        arithmetic(0, 0x3F800000, 0x33800000, mode); // 1 + 2^-24
        EXPECT_EQ(state.floating_point_registers[3], positive_sum[mode]);
        EXPECT_EQ(state.floating_point_control, mode | 0x00001004U);
        arithmetic(1, 0xBF800000, 0x33800000, mode); // -1 - 2^-24
        EXPECT_EQ(state.floating_point_registers[3], negative_sum[mode]);
        arithmetic(3, 0x3F800000, 0x40400000, mode); // 1 / 3
        EXPECT_EQ(state.floating_point_registers[3], quotient[mode]);
        arithmetic(4, 0x40000000, 0, mode); // sqrt(2)
        EXPECT_EQ(state.floating_point_registers[3], square_root[mode]);
        arithmetic(0x20, 16'777'217, 0, mode); // cvt.s.w
        EXPECT_EQ(state.floating_point_registers[3], integer_to_float[mode]);
        EXPECT_EQ(state.floating_point_control, mode | 0x00001004U);
        arithmetic(0x24, 0x40200000, 0, mode); // cvt.w.s 2.5
        EXPECT_EQ(state.floating_point_registers[3], converted[mode]);
        for (const auto &entry :
             {std::array{0x0CU, 2U}, std::array{0x0DU, 2U}, std::array{0x0EU, 3U}, std::array{0x0FU, 2U}})
        {
            arithmetic(entry[0], 0x40200000, 0, mode);
            EXPECT_EQ(state.floating_point_registers[3], entry[1]);
            EXPECT_EQ(state.floating_point_control, mode | 0x00001004U);
        }
    }
}

TEST_F(FpuTest, ConversionsSaturateWithoutOutOfRangeIntegerCasts)
{
    for (const auto &entry : {std::array{0x4F000000U, 0x7FFFFFFFU}, std::array{0xCF000001U, 0x80000000U},
                              std::array{0x7F800000U, 0x7FFFFFFFU}, std::array{0xFF800000U, 0x80000000U},
                              std::array{0x7FC12345U, 0x7FFFFFFFU}, std::array{0xFFC12345U, 0x7FFFFFFFU}})
    {
        arithmetic(0x24, entry[0], 0);
        EXPECT_EQ(state.floating_point_registers[3], entry[1]);
        EXPECT_EQ(state.floating_point_control, 0x00010040U);
    }
    arithmetic(0x24, 0xCF000000, 0); // INT32_MIN is valid.
    EXPECT_EQ(state.floating_point_registers[3], 0x80000000U);
    EXPECT_EQ(state.floating_point_control, 0U);
}

TEST_F(FpuTest, RecordsAllExceptionClassesAndFlushesTinyResults)
{
    // Result and FCR31 values follow fcr.expected, roundmode.expected, and IEEE divide by zero.
    constexpr std::array cases{
        std::array{4U, 0xBF800000U, 0U, 0x7FC00000U, 0x00010040U},          // sqrt(-1): invalid
        std::array{3U, 0x3F800000U, 0U, 0x7F800000U, 0x00008020U},          // 1/0: divide by zero
        std::array{2U, 0x7F7FFFFFU, 0x7F7FFFFFU, 0x7F800000U, 0x00005014U}, // overflow + inexact
        std::array{3U, 0x00800000U, 0x40400000U, 0x002AAAABU, 0x0000300CU}, // underflow + inexact
        std::array{3U, 0x3F800000U, 0x40400000U, 0x3EAAAAABU, 0x00001004U}, // inexact
    };
    for (const auto &entry : cases)
    {
        arithmetic(entry[0], entry[1], entry[2]);
        EXPECT_EQ(state.floating_point_registers[3], entry[3]);
        EXPECT_EQ(state.floating_point_control, entry[4]);
    }
    arithmetic(2, 0x00800000, 0x3F000000); // Exact subnormal preserved without FS.
    EXPECT_EQ(state.floating_point_registers[3], 0x00400000U);
    for (const auto &entry : {std::array{0x00800000U, 0U}, std::array{0x80800000U, 0x80000000U}})
    {
        arithmetic(2, entry[0], 0x3F000000, 0x01000000);
        EXPECT_EQ(state.floating_point_registers[3], entry[1]);
        EXPECT_EQ(state.floating_point_control, 0x0100300CU);
    }
}

TEST_F(FpuTest, StickyFlagsAccumulateWhileEachOperationReplacesCauses)
{
    arithmetic(4, 0xBF800000, 0);
    state.floating_point_registers[1] = 0x3F800000;
    state.floating_point_registers[2] = 0x3F800000;
    instruction(0x460208C0);                              // add.s $f3, $f1, $f2 (exact)
    EXPECT_EQ(state.floating_point_control, 0x00000040U); // Invalid remains sticky, cause cleared.
    state.floating_point_registers[2] = 0;
    instruction(0x460208C3); // div.s: new divide-by-zero cause and flag.
    EXPECT_EQ(state.floating_point_control, 0x00008060U);
    state.floating_point_registers[1] = 0x7FBFFFFF;
    instruction(0x46000831); // c.un.s $f1, $f0: quiet compare accepts libc NAN.
    EXPECT_EQ(state.floating_point_control, 0x00800060U);
    instruction(0x46000839); // c.ngle.s: signaling predicate, exceptions disabled.
    EXPECT_EQ(state.floating_point_control, 0x00810060U);
}

TEST_F(FpuTest, PropagatesNanPayloadsAndBitOperationsPreserveExceptionStatus)
{
    arithmetic(0, 0xFFC12345, 0x7FC54321);
    EXPECT_EQ(state.floating_point_registers[3], 0xFFC12345U); // First NaN wins.
    arithmetic(2, 0x3F800000, 0x7F800001);
    EXPECT_EQ(state.floating_point_registers[3], 0x7FC00001U); // Quiet the payload.
    for (const auto &entry : {std::array{5U, 0x7F812345U}, std::array{6U, 0xFF812345U}, std::array{7U, 0x7F812345U}})
    {
        arithmetic(entry[0], 0xFF812345, 0, 0x0101F07C);
        EXPECT_EQ(state.floating_point_registers[3], entry[1]);
        EXPECT_EQ(state.floating_point_control, 0x0101F07CU);
    }
    state.floating_point_registers[1] = 0x40000000;
    state.floating_point_registers[2] = 0x40400000;
    state.floating_point_control = 0;
    instruction(0x46020840); // add.s $f1, $f1, $f2: destination aliases left.
    EXPECT_EQ(state.floating_point_registers[1], 0x40A00000U);
    instruction(0x46020881); // sub.s $f2, $f1, $f2: destination aliases right.
    EXPECT_EQ(state.floating_point_registers[2], 0x40000000U);
}

TEST_F(FpuTest, RestoresHostEnvironmentAndErrnoAfterSuccessAndGuestFaults)
{
    ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    ASSERT_EQ(std::feclearexcept(FE_ALL_EXCEPT), 0);
    ASSERT_EQ(std::feraiseexcept(FE_DIVBYZERO), 0);
    errno = ERANGE;
    arithmetic(0, 0x3F800000, 0x33800000, 2); // Guest upward despite host downward.
    EXPECT_EQ(state.floating_point_registers[3], 0x3F800001U);
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
    EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), FE_DIVBYZERO);
    EXPECT_EQ(errno, ERANGE);
    arithmetic(4, 0xBF800000, 0); // libm may set errno for sqrt(-1).
    EXPECT_EQ(errno, ERANGE);
    EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), FE_DIVBYZERO);
    state.registers[8] = 0;
    state.floating_point_registers[1] = 0xBF800000;
    state.floating_point_control = 0x00000800;           // Enable invalid-operation exception.
    instruction(0x10000007);                             // integer branch to current PC + 32
    memory.write_u32(state.program_counter, 0x460008C4); // sqrt.s in its delay slot
    const auto before = state;
    EXPECT_THROW(cpu.step(state), std::runtime_error);
    EXPECT_EQ(state, before);
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
    EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), FE_DIVBYZERO);
    EXPECT_EQ(errno, ERANGE);
    state.floating_point_control = 0; // Disable the trap and retry the same delay slot.
    cpu.step(state);
    EXPECT_EQ(state.program_counter, before.next_program_counter);
    EXPECT_EQ(state.floating_point_registers[3], 0x7FC00000U);
}

TEST_F(FpuTest, EnabledExceptionClassesAndCtcCausesFaultBeforeCommitting)
{
    for (const auto &entry : {std::array{0U, 0x3F800000U, 0x33800000U, 0x00000080U}, // inexact
                              std::array{3U, 0x00800000U, 0x40400000U, 0x00000100U}, // underflow
                              std::array{2U, 0x7F7FFFFFU, 0x7F7FFFFFU, 0x00000200U}, // overflow
                              std::array{3U, 0x3F800000U, 0U, 0x00000400U}})         // divide by zero
    {
        state = CpuState{.program_counter = GuestAddress{kProgramBase}};
        state.floating_point_registers[1] = entry[1];
        state.floating_point_registers[2] = entry[2];
        state.floating_point_control = entry[3];
        memory.write_u32(state.program_counter, 0x460208C0 | entry[0]);
        const auto before = state;
        EXPECT_THROW(cpu.step(state), std::runtime_error);
        EXPECT_EQ(state, before);
    }
    state.registers[8] = 0x00010800; // Invalid cause plus invalid enable.
    memory.write_u32(state.program_counter, 0x44C8F800);
    const auto before = state;
    EXPECT_THROW(cpu.step(state), std::runtime_error);
    EXPECT_EQ(state, before);
}

TEST_F(FpuTest, RejectsWrongFormatsAndReservedUnaryOperands)
{
    for (const auto word : {0x460008E0U,  // cvt.s.w requires word, not single, format
                            0x468008E4U,  // cvt.w.s requires single, not word, format
                            0x460208C4U,  // sqrt.s has a reserved ft operand
                            0x460008E1U}) // unsupported function
    {
        state = CpuState{.program_counter = GuestAddress{kProgramBase}};
        memory.write_u32(state.program_counter, word);
        const auto before = state;
        EXPECT_THROW(cpu.step(state), std::runtime_error);
        EXPECT_EQ(state, before);
    }
}

} // namespace psp
