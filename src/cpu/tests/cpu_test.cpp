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

constexpr std::uint32_t kProgramBase = 0x08800000;

struct BranchCase
{
    const char *name;
    std::uint32_t instruction;
    bool likely;
    bool links;
    // Expected conditions for rs = -1, 0, 1, INT32_MIN; rt = zero.
    std::array<bool, 4> taken;
};

struct AluInitialState
{
    std::uint32_t destination{};
    std::uint64_t accumulator{};
};

struct AluResult
{
    std::uint32_t destination;
    std::uint32_t target;
    std::uint32_t high;
    std::uint32_t low;
    std::uint32_t zero;
};

struct PartialWordCase
{
    std::uint32_t instruction;
    std::array<std::uint32_t, 4> expected;
};

void load_program(Memory &memory, std::initializer_list<std::uint32_t> instructions)
{
    auto address = kProgramBase;
    for (const auto instruction : instructions)
    {
        memory.write_u32(GuestAddress{address}, instruction);
        address += 4;
    }
}

void step_n(Cpu &cpu, CpuState &state, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        cpu.step(state);
    }
}

AluResult run_alu(std::uint32_t instruction, std::uint32_t left, std::uint32_t right, AluInitialState initial = {})
{
    Memory memory(GuestAddress{kProgramBase}, 64);
    const auto low = static_cast<std::uint32_t>(initial.accumulator);
    const auto high = static_cast<std::uint32_t>(initial.accumulator >> 32);
    load_program(memory, {
                             0x3C080000 | (left >> 16), 0x35080000 | (left & 0xFFFF), 0x3C090000 | (right >> 16),
                             0x35290000 | (right & 0xFFFF), 0x3C0A0000 | (initial.destination >> 16),
                             0x354A0000 | (initial.destination & 0xFFFF), 0x3C0B0000 | (low >> 16),
                             0x356B0000 | (low & 0xFFFF), 0x3C0C0000 | (high >> 16), 0x358C0000 | (high & 0xFFFF),
                             0x01600013, // mtlo $t3
                             0x01800011, // mthi $t4
                             instruction,
                             0x00005810, // mfhi $t3
                             0x00006012, // mflo $t4
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    step_n(cpu, state, 15);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 60);
    return {state.registers[10], state.registers[9], state.registers[11], state.registers[12], state.registers[0]};
}

} // namespace

TEST(CpuTest, SharedExecutorPreservesThreadStateAndDelaySlotSnapshots)
{
    Memory memory(GuestAddress{kProgramBase}, 64);
    load_program(memory, {
                             0x01090019, // multu $t0, $t1
                             0x11400003, // beq $t2, $zero, target at +20
                             0x254A0001, // addiu $t2, $t2, 1 (delay slot)
                             0x00005812, // mflo $t3 (untaken path)
                             0x00006010, // mfhi $t4
                             0x00005812, // mflo $t3 (taken path)
                             0x00006010, // mfhi $t4
                         });
    Cpu cpu(memory);
    CpuState first{.program_counter = GuestAddress{kProgramBase}};
    CpuState second{.program_counter = GuestAddress{kProgramBase}};
    first.registers[8] = 0xFFFFFFFF;
    first.registers[9] = 2;
    second.registers[8] = 3;
    second.registers[9] = 4;
    second.registers[10] = 10;

    cpu.step(first);
    cpu.step(second);
    cpu.step(first);
    cpu.step(second);
    // Copy while the branch target is pending, before executing its delay slot.
    auto snapshot = first;
    cpu.step(second);
    cpu.step(first);
    EXPECT_EQ(first.program_counter.value_of(), kProgramBase + 20);
    EXPECT_EQ(second.program_counter.value_of(), kProgramBase + 12);
    step_n(cpu, first, 2);
    step_n(cpu, second, 2);
    EXPECT_EQ(first.registers[10], 1U);
    EXPECT_EQ(first.registers[11], 0xFFFFFFFEU);
    EXPECT_EQ(first.registers[12], 1U);
    EXPECT_EQ(second.registers[10], 11U);
    EXPECT_EQ(second.registers[11], 12U);
    EXPECT_EQ(second.registers[12], 0U);

    snapshot.registers[10] = 100;
    step_n(cpu, snapshot, 3);
    EXPECT_EQ(snapshot.program_counter, first.program_counter);
    EXPECT_EQ(snapshot.registers[10], 101U);
    EXPECT_EQ(snapshot.registers[11], first.registers[11]);
    EXPECT_EQ(snapshot.registers[12], first.registers[12]);
    EXPECT_EQ(first.registers[10], 1U);
}

TEST(CpuTest, ExecutesArithmeticAndLogicalInstructions)
{
    Memory memory(GuestAddress{kProgramBase}, 64);
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
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);

    step_n(cpu, state, 9);

    EXPECT_EQ(state.registers[8], 0x12345678U);
    EXPECT_EQ(state.registers[9], 0xFFFFFFFFU);
    EXPECT_EQ(state.registers[10], 0x12345677U);
    EXPECT_EQ(state.registers[11], 0x12345679U);
    EXPECT_EQ(state.registers[12], 0x12345678U);
    EXPECT_EQ(state.registers[13], 0xFFFFFFFFU);
    EXPECT_EQ(state.registers[14], 0xEDCBA987U);
    EXPECT_EQ(state.registers[15], 0U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 36);
}

TEST(CpuTest, ExecutesComparisonsAndShifts)
{
    Memory memory(GuestAddress{kProgramBase}, 64);
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
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);

    step_n(cpu, state, 12);

    EXPECT_EQ(state.registers[10], 1U);
    EXPECT_EQ(state.registers[11], 0U);
    EXPECT_EQ(state.registers[12], 1U);
    EXPECT_EQ(state.registers[13], 0U);
    EXPECT_EQ(state.registers[14], 1U);
    EXPECT_EQ(state.registers[15], 0xFFFFFFFCU);
    EXPECT_EQ(state.registers[16], 0x3FFFFFFFU);
    EXPECT_EQ(state.registers[17], 0xFFFFFFFFU);
    EXPECT_EQ(state.registers[18], 0xFFF0U);
    EXPECT_EQ(state.registers[19], 0xFFFF0000U);
}

TEST(CpuTest, KeepsZeroRegisterConstant)
{
    Memory memory(GuestAddress{kProgramBase}, 20);
    load_program(memory, {
                             0x24080007, // addiu $t0, $zero, 7
                             0x3C00FFFF, // lui   $zero, 0xffff
                             0x24000001, // addiu $zero, $zero, 1
                             0x00000000, // nop
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);

    state.registers[0] = 99; // Public snapshot data cannot change guest $zero semantics.
    step_n(cpu, state, 4);

    EXPECT_EQ(state.registers[0], 0U);
    EXPECT_EQ(state.registers[8], 7U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 16);
}

TEST(CpuTest, LoadsAndStoresWordsThroughMemory)
{
    Memory memory(GuestAddress{kProgramBase}, 64);
    load_program(memory, {
                             0x3C080880, // lui   $t0, 0x0880
                             0x24091234, // addiu $t1, $zero, 0x1234
                             0xAD090020, // sw    $t1, 32($t0)
                             0x8D0A0020, // lw    $t2, 32($t0)
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);

    step_n(cpu, state, 4);

    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 32}), 0x1234U);
    EXPECT_EQ(state.registers[10], 0x1234U);
}

TEST(CpuTest, RejectsUnsupportedAndMisalignedInstructionsWithoutAdvancing)
{
    Memory memory(GuestAddress{kProgramBase}, 16);
    load_program(memory, {0xFFFFFFFF});
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);

    EXPECT_THROW(cpu.step(state), std::runtime_error);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase);

    memory.write_u32(GuestAddress{kProgramBase}, 0x8C080001); // lw $t0, 1($zero)
    EXPECT_THROW(cpu.step(state), std::invalid_argument);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase);
    state.program_counter = GuestAddress{kProgramBase + 1};
    EXPECT_THROW(cpu.step(state), std::invalid_argument);
    EXPECT_EQ(state.program_counter, GuestAddress{kProgramBase + 1});
}

TEST(CpuTest, LoadsAndStoresBytesAndHalfwords)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    load_program(memory, {
                             0x81090000, // lb $t1, 0($t0)
                             0x910A0000, // lbu $t2, 0($t0)
                             0x850B0000, // lh $t3, 0($t0)
                             0x950C0000, // lhu $t4, 0($t0)
                             0xA10D0002, // sb $t5, 2($t0)
                             0xA50E0004, // sh $t6, 4($t0)
                         });
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x123480FF);
    memory.write_u32(GuestAddress{kProgramBase + 68}, 0xAABBCCDD);
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    state.registers[8] = kProgramBase + 64;
    state.registers[13] = 0xABCDEF56;
    state.registers[14] = 0x12347890;
    step_n(cpu, state, 6);
    EXPECT_EQ(state.registers[9], 0xFFFFFFFFU);
    EXPECT_EQ(state.registers[10], 0xFFU);
    EXPECT_EQ(state.registers[11], 0xFFFF80FFU);
    EXPECT_EQ(state.registers[12], 0x80FFU);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x125680FFU);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), 0xAABB7890U);
}

TEST(CpuTest, RejectsMisalignedHalfwordsAndOutOfBoundsByteAccess)
{
    for (const std::uint32_t instruction : {0x85090001U, 0x95090001U, 0xA5090001U, 0x81090040U, 0xA1090040U})
    {
        SCOPED_TRACE(instruction);
        Memory memory(GuestAddress{kProgramBase}, 64);
        load_program(memory, {instruction});
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        Cpu cpu(memory);
        state.registers[8] = kProgramBase;
        state.registers[9] = 0x12345678;
        EXPECT_ANY_THROW(cpu.step(state));
        EXPECT_EQ(state.program_counter, GuestAddress{kProgramBase});
        EXPECT_EQ(state.registers[9], 0x12345678U);
        EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase}), instruction);
    }
}

TEST(CpuTest, PartialWordLoadsMergeEveryByteOffset)
{
    // Expected values come from the bundled LSU hardware output.
    const PartialWordCase cases[] = {
        {0x89090000, {0x110E0D0C, 0x22110D0C, 0x3322110C, 0x44332211}}, // lwl $t1, offset($t0)
        {0x99090000, {0x44332211, 0x0F443322, 0x0F0E4433, 0x0F0E0D44}}, // lwr $t1, offset($t0)
    };
    for (const auto &test : cases)
    {
        for (std::uint32_t offset = 0; offset < 4; ++offset)
        {
            for (const int displacement : {0, -4})
            {
                const auto immediate = static_cast<std::uint16_t>(displacement + static_cast<int>(offset));
                SCOPED_TRACE(test.instruction | immediate);
                Memory memory(GuestAddress{kProgramBase}, 128);
                load_program(memory, {test.instruction | immediate});
                memory.write_u32(GuestAddress{kProgramBase + 64}, 0x44332211);
                CpuState state{.program_counter = GuestAddress{kProgramBase}};
                state.registers[8] = kProgramBase + 64 - static_cast<std::uint32_t>(displacement);
                state.registers[9] = 0x0F0E0D0C;
                Cpu cpu(memory);
                cpu.step(state);
                EXPECT_EQ(state.registers[9], test.expected[offset]);
                EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x44332211U);
            }
        }
    }
}

TEST(CpuTest, PartialWordStoresMergeEveryByteOffset)
{
    const PartialWordCase cases[] = {
        {0xA9090000, {0x44332212, 0x44331234, 0x44123456, 0x12345678}}, // swl $t1, offset($t0)
        {0xB9090000, {0x12345678, 0x34567811, 0x56782211, 0x78332211}}, // swr $t1, offset($t0)
    };
    for (const auto &test : cases)
    {
        for (std::uint32_t offset = 0; offset < 4; ++offset)
        {
            for (const int displacement : {0, -4})
            {
                const auto immediate = static_cast<std::uint16_t>(displacement + static_cast<int>(offset));
                SCOPED_TRACE(test.instruction | immediate);
                Memory memory(GuestAddress{kProgramBase}, 128);
                load_program(memory, {test.instruction | immediate});
                memory.write_u32(GuestAddress{kProgramBase + 64}, 0x44332211);
                memory.write_u32(GuestAddress{kProgramBase + 68}, 0x88776655);
                CpuState state{.program_counter = GuestAddress{kProgramBase}};
                state.registers[8] = kProgramBase + 64 - static_cast<std::uint32_t>(displacement);
                state.registers[9] = 0x12345678;
                Cpu cpu(memory);
                cpu.step(state);
                EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), test.expected[offset]);
                EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), 0x88776655U);
                EXPECT_EQ(state.registers[9], 0x12345678U);
            }
        }
    }
}

TEST(CpuTest, PartialWordPairsLoadAndStoreAcrossAlignedWordsInEitherOrder)
{
    for (const bool reverse : {false, true})
    {
        SCOPED_TRACE(reverse);
        Memory memory(GuestAddress{kProgramBase}, 128);
        const std::uint32_t left_load = 0x89090003;   // lwl $t1, 3($t0)
        const std::uint32_t right_load = 0x99090000;  // lwr $t1, 0($t0)
        const std::uint32_t left_store = 0xA90A0003;  // swl $t2, 3($t0)
        const std::uint32_t right_store = 0xB90A0000; // swr $t2, 0($t0)
        if (reverse)
        {
            load_program(memory, {right_load, left_load, right_store, left_store});
        }
        else
        {
            load_program(memory, {left_load, right_load, left_store, right_store});
        }
        memory.write_bytes(GuestAddress{kProgramBase + 64}, Payload{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        state.registers[8] = kProgramBase + 65;
        state.registers[9] = 0xFFFFFFFF;
        state.registers[10] = 0xAABBCCDD;
        Cpu cpu(memory);
        step_n(cpu, state, 4);
        EXPECT_EQ(state.registers[9], 0x55443322U);
        EXPECT_EQ(memory.read_bytes(GuestAddress{kProgramBase + 64}, 8),
                  (Payload{0x11, 0xDD, 0xCC, 0xBB, 0xAA, 0x66, 0x77, 0x88}));
    }
}

TEST(CpuTest, PartialWordLoadsReadAliasedBaseBeforeWritingAndDiscardZeroWrites)
{
    for (const auto instruction : {0x89080000U, 0x99080000U, 0x89000000U, 0x99000000U})
    {
        SCOPED_TRACE(instruction);
        Memory memory(GuestAddress{kProgramBase}, 128);
        load_program(memory, {instruction});
        memory.write_u32(GuestAddress{kProgramBase + 64}, 0x44332211);
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        state.registers[8] = kProgramBase + 65;
        Cpu cpu(memory);
        cpu.step(state);
        if (instruction == 0x89080000U)
        {
            EXPECT_EQ(state.registers[8], 0x22110041U);
        }
        else if (instruction == 0x99080000U)
        {
            EXPECT_EQ(state.registers[8], 0x08443322U);
        }
        else
        {
            EXPECT_EQ(state.registers[0], 0U);
            EXPECT_EQ(state.registers[8], kProgramBase + 65);
        }
    }
}

TEST(CpuTest, PartialWordFaultsPreserveRegistersMemoryAndPendingControlFlow)
{
    for (const auto instruction : {0x89090000U, 0x99090000U, 0xA9090000U, 0xB9090000U, 0x89000000U, 0x99000000U})
    {
        SCOPED_TRACE(instruction);
        Memory memory(GuestAddress{kProgramBase}, 64);
        load_program(memory, {instruction});
        const auto original = memory.read_bytes(GuestAddress{kProgramBase}, 64);
        CpuState state{.program_counter = GuestAddress{kProgramBase},
                       .next_program_counter = GuestAddress{kProgramBase + 32}};
        state.registers[8] = kProgramBase + 64;
        state.registers[9] = 0x12345678;
        Cpu cpu(memory);
        EXPECT_THROW(cpu.step(state), std::out_of_range);
        EXPECT_EQ(state.program_counter, GuestAddress{kProgramBase});
        EXPECT_EQ(state.next_program_counter, GuestAddress{kProgramBase + 32});
        EXPECT_EQ(state.registers[9], 0x12345678U);
        EXPECT_EQ(memory.read_bytes(GuestAddress{kProgramBase}, 64), original);
    }
}

TEST(CpuTest, StoreConditionalFailsWithoutLoadLinkedAndDoesNotWriteMemory)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    load_program(memory, {0xE1090000, 0xE10A0004}); // sc $t1, 0($t0); sc $t2, 4($t0)
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
    memory.write_u32(GuestAddress{kProgramBase + 68}, 0x22222222);
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    state.registers[8] = kProgramBase + 64;
    state.registers[9] = 0xAAAAAAAA;
    state.registers[10] = 0xBBBBBBBB;
    Cpu cpu(memory);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.registers[9], 0U);
    EXPECT_EQ(state.registers[10], 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x11111111U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), 0x22222222U);
    EXPECT_FALSE(state.load_linked);
}

TEST(CpuTest, AllegrexLinkSurvivesOrdinaryMemoryAccessAndAnotherLoadLinked)
{
    // These intervening operations all preserve the link in llsc.expected.
    for (const auto between : {0U, 0xAD0B0000U, 0xAD0B0004U, 0x8D0C0000U, 0xC10C0004U})
    {
        SCOPED_TRACE(between);
        Memory memory(GuestAddress{kProgramBase}, 128);
        load_program(memory, {0xC10A0000, between, 0xE1090000}); // ll $t2, 0($t0); ...; sc $t1, 0($t0)
        memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
        memory.write_u32(GuestAddress{kProgramBase + 68}, 0x22222222);
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        state.registers[8] = kProgramBase + 64;
        state.registers[9] = 0xAAAAAAAA;
        state.registers[11] = 0x77;
        Cpu cpu(memory);
        step_n(cpu, state, 3);
        EXPECT_EQ(state.registers[10], 0x11111111U);
        EXPECT_EQ(state.registers[9], 1U);
        EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0xAAAAAAAAU);
        EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), between == 0xAD0B0004U ? 0x77U : 0x22222222U);
        EXPECT_TRUE(state.load_linked);
    }
}

TEST(CpuTest, AllegrexStoreConditionalCanRepeatAndUseDifferentAddresses)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    load_program(memory, {0xC10A0000, 0xE1090004, 0xE10B000C}); // ll; sc to word 1; sc to word 3
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
    memory.write_u32(GuestAddress{kProgramBase + 68}, 0x22222222);
    memory.write_u32(GuestAddress{kProgramBase + 76}, 0x44444444);
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    state.registers[8] = kProgramBase + 64;
    state.registers[9] = 0xAAAAAAAA;
    state.registers[11] = 0xBBBBBBBB;
    Cpu cpu(memory);
    step_n(cpu, state, 3);
    EXPECT_EQ(state.registers[9], 1U);
    EXPECT_EQ(state.registers[11], 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x11111111U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), 0xAAAAAAAAU);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 76}), 0xBBBBBBBBU);
    EXPECT_TRUE(state.load_linked);
}

TEST(CpuTest, SyscallClearsLinkAndLoadLinkedCanRearmIt)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    load_program(memory, {0xC10A0000, 0xC, 0xE1090000, 0xE1090004, 0xC10A0000, 0xE1090000});
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
    memory.write_u32(GuestAddress{kProgramBase + 68}, 0x22222222);
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    state.registers[8] = kProgramBase + 64;
    state.registers[9] = 0xAAAAAAAA;
    Cpu cpu(memory);
    cpu.step(state);
    ASSERT_TRUE(state.load_linked);
    ASSERT_EQ(cpu.step(state), (Syscall{0, GuestAddress{kProgramBase + 4}}));
    EXPECT_FALSE(state.load_linked);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.registers[9], 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x11111111U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 68}), 0x22222222U);
    cpu.step(state);
    state.registers[9] = 0xBBBBBBBB;
    cpu.step(state);
    EXPECT_EQ(state.registers[9], 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0xBBBBBBBBU);
}

TEST(CpuTest, LinkedLoadsAndStoresHandleZeroAndAliasedRegistersWithSignedOffsets)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    // LL to zero still links; SC from zero stores zero and discards its result.
    load_program(memory, {0xC100FFFC, 0xE100FFFC, 0xC108FFFC});
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    state.registers[8] = kProgramBase + 68;
    Cpu cpu(memory);
    cpu.step(state);
    EXPECT_TRUE(state.load_linked);
    EXPECT_EQ(state.registers[0], 0U);
    cpu.step(state);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0U);
    EXPECT_EQ(state.registers[0], 0U);
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x22222222);
    cpu.step(state); // LL overwrites its address base only after reading memory.
    EXPECT_EQ(state.registers[8], 0x22222222U);

    load_program(memory, {0xE108FFFC}); // SC uses old rt/base for both address and value.
    state.program_counter = GuestAddress{kProgramBase};
    state.next_program_counter = GuestAddress{kProgramBase + 4};
    state.registers[8] = kProgramBase + 68;
    cpu.step(state);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), kProgramBase + 68);
    EXPECT_EQ(state.registers[8], 1U);
}

TEST(CpuTest, LinkedMemoryFaultsPreserveStateAndMemory)
{
    for (const auto instruction : {0xC1090001U, 0xC1090040U, 0xE1090001U, 0xE1090040U})
    {
        for (const bool linked : {false, true})
        {
            // An unlinked SC does not access memory, so test its alignment fault only.
            if (!linked && instruction == 0xE1090040U)
            {
                continue;
            }
            SCOPED_TRACE(instruction);
            SCOPED_TRACE(linked);
            Memory memory(GuestAddress{kProgramBase}, 64);
            load_program(memory, {instruction});
            const auto original_memory = memory.read_bytes(GuestAddress{kProgramBase}, 64);
            CpuState state{.program_counter = GuestAddress{kProgramBase},
                           .next_program_counter = GuestAddress{kProgramBase + 32}};
            state.registers[8] = kProgramBase;
            state.registers[9] = 0xAAAAAAAA;
            state.load_linked = linked;
            const auto original_state = state;
            Cpu cpu(memory);
            EXPECT_ANY_THROW(cpu.step(state));
            EXPECT_EQ(state, original_state);
            EXPECT_EQ(memory.read_bytes(GuestAddress{kProgramBase}, 64), original_memory);
        }
    }
}

TEST(CpuTest, LinkStateBelongsToTheSuppliedCpuState)
{
    Memory memory(GuestAddress{kProgramBase}, 128);
    load_program(memory, {0xC10A0000, 0xE1090000});
    memory.write_u32(GuestAddress{kProgramBase + 64}, 0x11111111);
    CpuState first{.program_counter = GuestAddress{kProgramBase}};
    first.registers[8] = kProgramBase + 64;
    first.registers[9] = 0xAAAAAAAA;
    CpuState second{.program_counter = GuestAddress{kProgramBase + 4}};
    second.registers[8] = kProgramBase + 64;
    second.registers[9] = 0xBBBBBBBB;
    Cpu cpu(memory);
    cpu.step(first);
    auto saved = first;
    cpu.step(second);
    EXPECT_EQ(second.registers[9], 0U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0x11111111U);
    cpu.step(saved);
    EXPECT_EQ(saved.registers[9], 1U);
    EXPECT_EQ(memory.read_u32(GuestAddress{kProgramBase + 64}), 0xAAAAAAAAU);
    EXPECT_TRUE(first.load_linked);
}

TEST(CpuTest, ReportsSyscallsAfterCommittingTheReturnDelaySlot)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {0x03E00008, (123U << 6) | 0xC}); // jr $ra; syscall 123
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    state.registers[31] = kProgramBase + 16;
    state.load_linked = true;
    EXPECT_FALSE(cpu.step(state));
    EXPECT_EQ(state.program_counter, GuestAddress{kProgramBase + 4});
    EXPECT_TRUE(state.load_linked);
    EXPECT_EQ(cpu.step(state), (Syscall{123U, GuestAddress{kProgramBase + 4}}));
    EXPECT_EQ(state.program_counter, GuestAddress{kProgramBase + 16});
    EXPECT_FALSE(state.load_linked);
}

TEST(CpuTest, ReportsZeroAndMaximumSyscallCodesWithTheirInstructionAddresses)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {0xC, (0xFFFFFU << 6) | 0xC, 0});
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    EXPECT_EQ(cpu.step(state), (Syscall{0, GuestAddress{kProgramBase}}));
    EXPECT_EQ(cpu.step(state), (Syscall{0xFFFFF, GuestAddress{kProgramBase + 4}}));
    EXPECT_FALSE(cpu.step(state));
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
            Memory memory(GuestAddress{kProgramBase}, 64);
            load_program(memory, {
                                     0x3C080000 | (values[index] >> 16),    // lui $t0, upper half
                                     0x35080000 | (values[index] & 0xFFFF), // ori $t0, $t0, lower half
                                     test.instruction,                      // branch to base + 20
                                     0x27EA0001,                            // addiu $t2, $ra, 1 (delay slot)
                                     0x240B0001,                            // addiu $t3, $zero, 1 (fallthrough)
                                     0x240C0001,                            // addiu $t4, $zero, 1 (target)
                                 });
            CpuState state{.program_counter = GuestAddress{kProgramBase}};
            Cpu cpu(memory);
            step_n(cpu, state, 3);

            const bool skipped = test.likely && !test.taken[index];
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + (skipped ? 16 : 12));
            EXPECT_EQ(state.registers[31], test.links ? kProgramBase + 16 : 0U);
            EXPECT_EQ(state.registers[10], 0U);
            if (!skipped)
            {
                cpu.step(state);
                EXPECT_EQ(state.registers[10], test.links ? kProgramBase + 17 : 1U);
            }
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + (test.taken[index] ? 20 : 16));
            cpu.step(state);
            EXPECT_EQ(state.registers[11], test.taken[index] ? 0U : 1U);
            EXPECT_EQ(state.registers[12], test.taken[index] ? 1U : 0U);
            EXPECT_EQ(state.program_counter.value_of(), kProgramBase + (test.taken[index] ? 24 : 20));
        }
    }
}

TEST(CpuTest, BranchConditionIsEvaluatedBeforeDelaySlot)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {
                             0x11000002, // beq $t0, $zero, base + 12
                             0x24080001, // addiu $t0, $zero, 1
                             0x24090001, // addiu $t1, $zero, 1 (skipped)
                             0x00000000,
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.registers[8], 1U);
    EXPECT_EQ(state.registers[9], 0U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 12);
}

TEST(CpuTest, ExecutesBackwardLoopWithDelaySlots)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {
                             0x24080003, // addiu $t0, $zero, 3
                             0x2508FFFF, // addiu $t0, $t0, -1
                             0x1500FFFE, // bne $t0, $zero, base + 4
                             0x25290001, // addiu $t1, $t1, 1 (runs on all three iterations)
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    step_n(cpu, state, 10);
    EXPECT_EQ(state.registers[8], 0U);
    EXPECT_EQ(state.registers[9], 3U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 16);
}

TEST(CpuTest, CallsFunctionAndReturnsAfterBothDelaySlots)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {
                             0x0E200004, // jal base + 16
                             0x24040007, // addiu $a0, $zero, 7 (call delay slot)
                             0x244B0000, // addiu $t3, $v0, 0 (return address)
                             0x00000000,
                             0x24820001, // addiu $v0, $a0, 1
                             0x03E00008, // jr $ra
                             0x24420002, // addiu $v0, $v0, 2 (return delay slot)
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 4);
    EXPECT_EQ(state.registers[31], kProgramBase + 8);
    EXPECT_EQ(state.registers[4], 0U);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 16);
    EXPECT_EQ(state.registers[4], 7U);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 24);
    EXPECT_EQ(state.registers[2], 8U);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 8);
    EXPECT_EQ(state.registers[2], 10U);
    cpu.step(state);
    EXPECT_EQ(state.registers[11], 10U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 12);
}

TEST(CpuTest, JalrCapturesTargetBeforeWritingAnyLinkRegister)
{
    // Link to a different register, to the source register, or to $zero.
    for (const std::uint32_t destination : {9U, 8U, 0U})
    {
        SCOPED_TRACE(destination);
        Memory memory(GuestAddress{kProgramBase}, 32);
        load_program(memory, {
                                 0x3C080880,                       // lui $t0, 0x0880
                                 0x35080018,                       // ori $t0, $t0, 24
                                 0x01000009 | (destination << 11), // jalr rd, $t0
                                 0x250A0000,                       // addiu $t2, $t0, 0 (delay slot reads updated link)
                                 0xFFFFFFFF,                       // must be skipped
                                 0xFFFFFFFF,
                                 0x240B0007, // addiu $t3, $zero, 7
                             });
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        Cpu cpu(memory);
        step_n(cpu, state, 3);
        EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 12);
        EXPECT_EQ(state.registers[destination], destination == 0 ? 0U : kProgramBase + 16);
        EXPECT_EQ(state.registers[31], 0U);
        cpu.step(state);
        EXPECT_EQ(state.registers[10], kProgramBase + (destination == 8 ? 16 : 24));
        EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 24);
        cpu.step(state);
        EXPECT_EQ(state.registers[11], 7U);
    }
}

TEST(CpuTest, JumpUsesUpperBitsOfPcPlusFour)
{
    // Cross a 256 MiB boundary: PC and PC + 4 have different upper bits.
    Memory memory(GuestAddress{0x0FFFFFFC}, 12);
    memory.write_u32(GuestAddress{0x0FFFFFFC}, 0x08000001); // j 0x10000004
    memory.write_u32(GuestAddress{0x10000000}, 0x24080001); // delay slot
    memory.write_u32(GuestAddress{0x10000004}, 0x24090002);
    CpuState state{.program_counter = GuestAddress{0x0FFFFFFC}};
    Cpu cpu(memory);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), 0x10000000U);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), 0x10000004U);
    EXPECT_EQ(state.registers[8], 1U);
    EXPECT_EQ(state.registers[31], 0U);
    cpu.step(state);
    EXPECT_EQ(state.registers[9], 2U);
}

TEST(CpuTest, UntakenLikelyBranchDoesNotExecuteInvalidDelaySlot)
{
    Memory memory(GuestAddress{kProgramBase}, 16);
    load_program(memory, {
                             0x54000002, // bnel $zero, $zero, base + 12 (untaken)
                             0xFFFFFFFF, // invalid delay slot must not be decoded
                             0x24080001,
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 8);
    cpu.step(state);
    EXPECT_EQ(state.registers[8], 1U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 12);
}

TEST(CpuTest, FailedDelaySlotPreservesPendingBranchForRetry)
{
    Memory memory(GuestAddress{kProgramBase}, 20);
    load_program(memory, {0x10000002, 0xFFFFFFFF, 0xFFFFFFFF, 0x24080007});
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    cpu.step(state);
    const auto pending = state;
    EXPECT_THROW(cpu.step(state), std::runtime_error);
    EXPECT_EQ(state, pending);
    memory.write_u32(GuestAddress{kProgramBase + 4}, 0x8C080001); // misaligned lw
    EXPECT_THROW(cpu.step(state), std::invalid_argument);
    EXPECT_EQ(state, pending);
    memory.write_u32(GuestAddress{kProgramBase + 4}, 0x00000000); // repair delay slot
    cpu.step(state);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 12);
    cpu.step(state);
    EXPECT_EQ(state.registers[8], 7U);
}

TEST(CpuTest, MisalignedRegisterJumpFaultsAfterExecutingDelaySlot)
{
    Memory memory(GuestAddress{kProgramBase}, 24);
    load_program(memory, {
                             0x3C080880,
                             0x35080001, // $t0 = base + 1
                             0x01000008, // jr $t0
                             0x24090007,
                         });
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    step_n(cpu, state, 4);
    EXPECT_EQ(state.registers[9], 7U);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 1);
    EXPECT_THROW(cpu.step(state), std::invalid_argument);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 1);
}

TEST(CpuTest, RejectsUnknownRegimmWithoutChangingControlFlow)
{
    Memory memory(GuestAddress{kProgramBase}, 16);
    load_program(memory, {0x041F0001}); // unsupported REGIMM operation
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    EXPECT_THROW(cpu.step(state), std::runtime_error);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase);
    EXPECT_EQ(state.registers[31], 0U);
    memory.write_u32(GuestAddress{kProgramBase}, 0x00000000);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 8);
}

TEST(CpuTest, ExecutesVariableShiftsAndRotationsWithMaskedCounts)
{
    struct ShiftCase
    {
        std::uint32_t count;
        std::uint32_t left;
        std::uint32_t right;
        std::uint32_t arithmetic;
        std::uint32_t rotate;
    };
    const ShiftCase cases[] = {
        {0, 0x80000001, 0x80000001, 0x80000001, 0x80000001},
        {1, 0x00000002, 0x40000000, 0xC0000000, 0xC0000000},
        {31, 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000003},
        {32, 0x80000001, 0x80000001, 0x80000001, 0x80000001},
        {33, 0x00000002, 0x40000000, 0xC0000000, 0xC0000000},
        {0xFFFFFFFF, 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000003},
    };
    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.count);
        EXPECT_EQ(run_alu(0x01095004, test.count, 0x80000001).destination, test.left);       // sllv
        EXPECT_EQ(run_alu(0x01095006, test.count, 0x80000001).destination, test.right);      // srlv
        EXPECT_EQ(run_alu(0x01095007, test.count, 0x80000001).destination, test.arithmetic); // srav
        EXPECT_EQ(run_alu(0x01095046, test.count, 0x80000001).destination, test.rotate);     // rotrv
    }
    EXPECT_EQ(run_alu(0x00295002, 0, 0x80000001).destination, 0x80000001U); // rotr by 0
    EXPECT_EQ(run_alu(0x00295102, 0, 0x12345678).destination, 0x81234567U); // rotr by 4
    EXPECT_EQ(run_alu(0x002957C2, 0, 0x80000001).destination, 3U);          // rotr by 31
    EXPECT_EQ(run_alu(0x01094846, 4, 0x12345678).target, 0x81234567U);      // rotrv with rt == rd
}

TEST(CpuTest, ConditionalMovesPreserveDestinationWhenConditionFails)
{
    EXPECT_EQ(run_alu(0x0109500A, 0x12345678, 0, {.destination = 7}).destination, 0x12345678U); // movz
    EXPECT_EQ(run_alu(0x0109500A, 0x12345678, 1, {.destination = 7}).destination, 7U);
    EXPECT_EQ(run_alu(0x0109500B, 0x12345678, 0, {.destination = 7}).destination, 7U); // movn
    EXPECT_EQ(run_alu(0x0109500B, 0x12345678, 0x80000000, {.destination = 7}).destination, 0x12345678U);
    EXPECT_EQ(run_alu(0x0109480B, 0x12345678, 1).target, 0x12345678U); // movn with rt == rd
}

TEST(CpuTest, ExecutesSignedMinAndMax)
{
    EXPECT_EQ(run_alu(0x0109502C, 0x80000000, 0x7FFFFFFF).destination, 0x7FFFFFFFU); // max
    EXPECT_EQ(run_alu(0x0109502D, 0x80000000, 0x7FFFFFFF).destination, 0x80000000U); // min
    EXPECT_EQ(run_alu(0x0109502C, 0xFFFFFFFF, 0).destination, 0U);
    EXPECT_EQ(run_alu(0x0109502D, 0, 0xFFFFFFFF).destination, 0xFFFFFFFFU);
    EXPECT_EQ(run_alu(0x0109502C, 7, 7).destination, 7U);
    EXPECT_EQ(run_alu(0x0109502D, 7, 7).destination, 7U);
}

TEST(CpuTest, CountsLeadingBitsUsingAllegrexEncodings)
{
    EXPECT_EQ(run_alu(0x01005016, 0, 0).destination, 32U); // clz
    EXPECT_EQ(run_alu(0x01005016, 1, 0).destination, 31U);
    EXPECT_EQ(run_alu(0x01005016, 0x80000000, 0).destination, 0U);
    EXPECT_EQ(run_alu(0x01005016, 0x00008000, 0).destination, 16U);
    EXPECT_EQ(run_alu(0x01005017, 0xFFFFFFFF, 0).destination, 32U); // clo
    EXPECT_EQ(run_alu(0x01005017, 0xFFFFFFFE, 0).destination, 31U);
    EXPECT_EQ(run_alu(0x01005017, 0, 0).destination, 0U);
    EXPECT_EQ(run_alu(0x01005017, 0xFFFF0000, 0).destination, 16U);
}

TEST(CpuTest, SignExtendsLowByteAndHalfword)
{
    EXPECT_EQ(run_alu(0x7C095420, 0, 0x12345681).destination, 0xFFFFFF81U); // seb
    EXPECT_EQ(run_alu(0x7C095420, 0, 0xFFFFFF7F).destination, 0x7FU);
    EXPECT_EQ(run_alu(0x7C095420, 0, 0x12345600).destination, 0U);
    EXPECT_EQ(run_alu(0x7C095620, 0, 0x12348123).destination, 0xFFFF8123U); // seh
    EXPECT_EQ(run_alu(0x7C095620, 0, 0xFFFF7FFF).destination, 0x7FFFU);
    EXPECT_EQ(run_alu(0x7C095620, 0, 0xFFFF0000).destination, 0U);
}

TEST(CpuTest, SwapsBytesAndReversesBits)
{
    EXPECT_EQ(run_alu(0x7C0950A0, 0, 0x12345678).destination, 0x34127856U); // wsbh
    EXPECT_EQ(run_alu(0x7C0950E0, 0, 0x12345678).destination, 0x78563412U); // wsbw
    EXPECT_EQ(run_alu(0x7C095520, 0, 0).destination, 0U);                   // bitrev
    EXPECT_EQ(run_alu(0x7C095520, 0, 0xFFFFFFFF).destination, 0xFFFFFFFFU);
    EXPECT_EQ(run_alu(0x7C095520, 0, 1).destination, 0x80000000U);
    EXPECT_EQ(run_alu(0x7C095520, 0, 0x80000000).destination, 1U);
    EXPECT_EQ(run_alu(0x7C095520, 0, 0x12345678).destination, 0x1E6A2C48U);
    EXPECT_EQ(run_alu(0x7C0948E0, 0, 0x12345678).target, 0x78563412U); // wsbw with rt == rd
}

TEST(CpuTest, ExtractsAndInsertsBitfieldsIncludingFullWord)
{
    EXPECT_EQ(run_alu(0x7D098180, 0xFEDCBA98, 0).target, 0x172EAU);    // ext $t1, $t0, 6, 17
    EXPECT_EQ(run_alu(0x7D098104, 0xFFFFFFFF, 0).target, 0x0001FFF0U); // ins $t1, $t0, 4, 13
    EXPECT_EQ(run_alu(0x7D098104, 0, 0xFFFFFFFF).target, 0xFFFE000FU);
    EXPECT_EQ(run_alu(0x7D09F800, 0x12345678, 0).target, 0x12345678U);          // ext full word
    EXPECT_EQ(run_alu(0x7D09F804, 0x12345678, 0xFFFFFFFF).target, 0x12345678U); // ins full word
    EXPECT_EQ(run_alu(0x7D0907C0, 0x80000000, 0).target, 1U);                   // ext top bit
    EXPECT_EQ(run_alu(0x7D09FFC4, 1, 0).target, 0x80000000U);                   // ins top bit
    EXPECT_EQ(run_alu(0x7D298104, 0, 0x12345678).target, 0x12356788U);          // ins with rs == rt
}

TEST(CpuTest, MovesHiLoIndependentlyAndStartsThemAtZero)
{
    Memory memory(GuestAddress{kProgramBase}, 32);
    load_program(memory, {0x00004010, 0x00004812}); // mfhi $t0; mflo $t1
    CpuState state{.program_counter = GuestAddress{kProgramBase}};
    Cpu cpu(memory);
    step_n(cpu, state, 2);
    EXPECT_EQ(state.registers[8], 0U);
    EXPECT_EQ(state.registers[9], 0U);

    const auto high = run_alu(0x01000011, 0x12345678, 0, {.accumulator = 0xABCDEF0198765432ULL}); // mthi
    EXPECT_EQ(high.high, 0x12345678U);
    EXPECT_EQ(high.low, 0x98765432U);
    const auto low = run_alu(0x01000013, 0x12345678, 0, {.accumulator = 0xABCDEF0198765432ULL}); // mtlo
    EXPECT_EQ(low.high, 0xABCDEF01U);
    EXPECT_EQ(low.low, 0x12345678U);
    EXPECT_EQ(run_alu(0x00005010, 0, 0, {.accumulator = 0xABCDEF0198765432ULL}).destination, 0xABCDEF01U); // mfhi
    EXPECT_EQ(run_alu(0x00005012, 0, 0, {.accumulator = 0xABCDEF0198765432ULL}).destination, 0x98765432U); // mflo
}

TEST(CpuTest, MultipliesAndAccumulatesWithWrappingHiLo)
{
    struct MultiplyCase
    {
        std::uint32_t instruction;
        std::uint32_t left;
        std::uint32_t right;
        std::uint64_t accumulator;
        std::uint64_t expected;
    };
    const MultiplyCase cases[] = {
        {0x01090018, 0xFFFFFFFF, 2, 7, 0xFFFFFFFFFFFFFFFEULL}, // mult: -1 * 2
        {0x01090018, 0x80000000, 0x80000000, 7, 0x4000000000000000ULL},
        {0x01090019, 0xFFFFFFFF, 2, 7, 0x00000001FFFFFFFEULL}, // multu
        {0x01090019, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFE00000001ULL},
        {0x01090018, 0, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFFULL, 0},
        {0x0109001C, 0xFFFFFFFF, 2, 1, 0xFFFFFFFFFFFFFFFFULL}, // madd
        {0x0109001C, 1, 1, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL},
        {0x0109001C, 1, 1, 0xFFFFFFFFFFFFFFFFULL, 0},
        {0x0109001D, 0xFFFFFFFF, 2, 1, 0x00000001FFFFFFFFULL}, // maddu
        {0x0109001D, 1, 1, 0x00000000FFFFFFFFULL, 0x0000000100000000ULL},
        {0x0109001D, 1, 1, 0xFFFFFFFFFFFFFFFFULL, 0},
        {0x0109002E, 0xFFFFFFFF, 2, 0, 2}, // msub subtracts a signed product
        {0x0109002E, 1, 1, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL},
        {0x0109002F, 0xFFFFFFFF, 2, 0, 0xFFFFFFFE00000002ULL}, // msubu
        {0x0109002F, 1, 1, 0x0000000100000000ULL, 0x00000000FFFFFFFFULL},
    };
    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.instruction);
        SCOPED_TRACE(test.accumulator);
        const auto result = run_alu(test.instruction, test.left, test.right, {.accumulator = test.accumulator});
        EXPECT_EQ(result.high, static_cast<std::uint32_t>(test.expected >> 32));
        EXPECT_EQ(result.low, static_cast<std::uint32_t>(test.expected));
    }
}

TEST(CpuTest, DividesWithAllegrexOverflowAndZeroDivisorResults)
{
    struct DivideCase
    {
        std::uint32_t instruction;
        std::uint32_t numerator;
        std::uint32_t denominator;
        std::uint32_t quotient;
        std::uint32_t remainder;
    };
    // Special cases match third_party/pspautotests/tests/cpu/cpu_alu/cpu_div.expected.
    const DivideCase cases[] = {
        {0x0109001A, 100, 7, 14, 2},
        {0x0109001A, 0xFFFFFF9C, 7, 0xFFFFFFF2, 0xFFFFFFFE},
        {0x0109001A, 100, 0xFFFFFFF9, 0xFFFFFFF2, 2},
        {0x0109001A, 0xFFFFFF9C, 0xFFFFFFF9, 14, 0xFFFFFFFE},
        {0x0109001A, 0x80000000, 0xFFFFFFFF, 0x80000000, 0},
        {0x0109001A, 0x80000000, 0xFFFFFFFE, 0x40000000, 0},
        {0x0109001A, 0, 0, 0xFFFFFFFF, 0},
        {0x0109001A, 1, 0, 0xFFFFFFFF, 1},
        {0x0109001A, 0xFFFFFFFF, 0, 1, 0xFFFFFFFF},
        {0x0109001A, 0x80000000, 0, 1, 0x80000000},
        {0x0109001B, 0xFFFFFFFF, 7, 0x24924924, 3},
        {0x0109001B, 0x80000000, 0xFFFFFFFF, 0, 0x80000000},
        {0x0109001B, 0, 0, 0xFFFF, 0},
        {0x0109001B, 1, 0, 0xFFFF, 1},
        {0x0109001B, 0xFFFF, 0, 0xFFFF, 0xFFFF},
        {0x0109001B, 0x10000, 0, 0xFFFFFFFF, 0x10000},
        {0x0109001B, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF},
    };
    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.instruction);
        SCOPED_TRACE(test.numerator);
        SCOPED_TRACE(test.denominator);
        const auto result =
            run_alu(test.instruction, test.numerator, test.denominator, {.accumulator = 0x123456789ABCDEF0ULL});
        EXPECT_EQ(result.low, test.quotient);
        EXPECT_EQ(result.high, test.remainder);
    }
}

TEST(CpuTest, NewAluInstructionsDiscardWritesToZero)
{
    const std::uint32_t instructions[] = {
        0x00290002, // rotr $zero, $t1, 0
        0x01090004, // sllv $zero, $t1, $t0
        0x01090006, // srlv
        0x01090046, // rotrv
        0x01090007, // srav
        0x0109000A, // movz
        0x0109000B, // movn
        0x00000010, // mfhi
        0x00000012, // mflo
        0x01000016, // clz
        0x01000017, // clo
        0x0109002C, // max
        0x0109002D, // min
        0x7C0900A0, // wsbh
        0x7C0900E0, // wsbw
        0x7C090420, // seb
        0x7C090520, // bitrev
        0x7C090620, // seh
        0x7D000000, // ext
        0x7D000004, // ins
    };
    for (const auto instruction : instructions)
    {
        SCOPED_TRACE(instruction);
        const auto result =
            run_alu(instruction, 0xFFFFFFFF, 0xFFFFFFFF, {.destination = 7, .accumulator = 0x123456789ABCDEF0ULL});
        EXPECT_EQ(result.zero, 0U);
        EXPECT_EQ(result.high, 0x12345678U);
        EXPECT_EQ(result.low, 0x9ABCDEF0U);
    }
}

TEST(CpuTest, RejectsInvalidAluEncodingsWithoutAdvancing)
{
    const std::uint32_t instructions[] = {
        0x00495002, // invalid SRL/ROTR selector
        0x01095086, // invalid SRLV/ROTRV selector
        0x7D09F840, // ext width 32 at position 1
        0x7D090044, // ins end 0 before start 1
        0x7C095120, // unknown BSHFL operation
        0x7C00003F, // unknown SPECIAL3 function
    };
    for (const auto instruction : instructions)
    {
        SCOPED_TRACE(instruction);
        Memory memory(GuestAddress{kProgramBase}, 16);
        load_program(memory, {instruction});
        CpuState state{.program_counter = GuestAddress{kProgramBase}};
        Cpu cpu(memory);
        EXPECT_THROW(cpu.step(state), std::exception);
        EXPECT_EQ(state.program_counter.value_of(), kProgramBase);
        EXPECT_EQ(state.registers[9], 0U);
        EXPECT_EQ(state.registers[10], 0U);
        memory.write_u32(GuestAddress{kProgramBase}, 0x00000000);
        step_n(cpu, state, 2);
        EXPECT_EQ(state.program_counter.value_of(), kProgramBase + 8);
    }
}

} // namespace psp
