#include "runtime/execution.hpp"

#include "loader/tests/prx_fixture.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>

namespace psp
{

namespace
{

// Small guest program with up to three service imports and a separate work area.
class ServicePrxFixture
{
public:
    ServicePrxFixture(const std::string &library, std::initializer_list<std::uint32_t> identifiers)
    {
        fixture.word(24, 0x300);
        fixture.word(68, 0x480);
        fixture.word(72, 0x500);
        fixture.word(0x180, 0xD0);
        fixture.name(0x1D0, library);
        fixture.halfword(0x18A, static_cast<std::uint16_t>(identifiers.size()));
        fixture.word(0x18C, 0x100);
        fixture.word(0x190, 0x120);
        std::size_t index = 0;
        for (const auto identifier : identifiers)
        {
            fixture.word(0x200 + index * 4, identifier);
            ++index;
        }
        instruction(0x03E0B821); // move $s7, $ra
    }

    void instruction(std::uint32_t word)
    {
        fixture.word(offset, word);
        offset += 4;
    }

    // The argument register index and its immediate value are separate encoding fields.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void argument(unsigned index, std::uint32_t value)
    {
        const auto reg = 4 + index;
        instruction(0x3C000000 | (reg << 16) | (value >> 16));
        instruction(0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF));
    }

    void call(unsigned index)
    {
        fixture.relocation(static_cast<std::uint32_t>(offset - 0x100), 4);
        instruction(0x0C000048 + index * 2);
        instruction(0); // nop in call delay slot
    }

    ParsedPrx finish()
    {
        instruction(0x02E0F821); // move $ra, $s7
        instruction(0x03E00008); // jr $ra
        instruction(0);
        return read_prx(fixture.bytes);
    }

    test::PrxFixture fixture;
    std::size_t offset{0x400};
};

} // namespace

class CpuPrxExecutionTest : public testing::TestWithParam<const char *>
{
};

TEST_P(CpuPrxExecutionTest, MatchesEntireHardwareOutput)
{
    const std::string directory = std::string(PSPAUTOTESTS_ROOT) + "/tests/cpu/cpu_alu/";
    std::ifstream input(directory + GetParam() + ".prx", std::ios::binary);
    std::ifstream expected_file(directory + GetParam() + ".expected", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    ASSERT_TRUE(expected_file.is_open());
    const Payload payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const std::string expected{std::istreambuf_iterator<char>(expected_file), std::istreambuf_iterator<char>()};
    const auto parsed = read_prx(payload);
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        ExecutionOptions options;
        options.load_address = GuestAddress{base};
        const auto result = execute_prx(parsed, options);
        EXPECT_EQ(result.exit_code, 0);
        EXPECT_EQ(result.output, expected);
        EXPECT_GT(result.instructions_executed, 0U);
        EXPECT_LE(result.instructions_executed, options.max_instructions);
    }
}

INSTANTIATE_TEST_SUITE_P(BundledCpu, CpuPrxExecutionTest, testing::Values("cpu_alu", "cpu_branch2", "cpu_div"));

TEST(ExecutionTest, RejectsUnboundSyscallCodes)
{
    for (const std::uint32_t code : {0U, 2U, 0xFFFFFU})
    {
        SCOPED_TRACE(code);
        ServicePrxFixture program("StdioForUser", {0xA6BAB2E9});
        program.instruction((code << 6) | 0xC);
        try
        {
            execute_prx(program.finish());
            FAIL() << "Unbound syscall codes must fail";
        }
        catch (const std::runtime_error &error)
        {
            EXPECT_NE(std::string(error.what()).find("Unbound syscall"), std::string::npos);
        }
    }
}

TEST(ExecutionTest, CreatesLightweightMutexWorkAreaAndDeletesItsIdentity)
{
    ServicePrxFixture program("ThreadManForUser", {0x19CFF145, 0x60107536});
    program.fixture.name(0x280, "mutex");
    program.argument(0, 0x08800200);
    program.argument(1, 0x08800180);
    program.argument(2, 0x200); // recursive
    program.argument(3, 2);     // initially owned by the creating thread
    program.argument(4, 0);
    program.call(0);
    program.instruction(0x8C820000); // lw $v0, 0($a0): initial lock count
    auto parsed = program.finish();
    EXPECT_EQ(execute_prx(parsed).exit_code, 2);

    // Replace the return sequence with a delete and read back its invalidated UID.
    program.offset -= 12;
    program.call(1);
    program.instruction(0x8C820010); // lw $v0, 16($a0)
    parsed = program.finish();
    EXPECT_EQ(execute_prx(parsed).exit_code, -1);
    program.fixture.word(0x400 + 5 * 4, 0x3C060001); // invalid creation attributes
    EXPECT_THROW(execute_prx(read_prx(program.fixture.bytes)), std::runtime_error);
}

TEST(ExecutionTest, LightweightMutexLocksTrackOwnershipAndRejectUnderflow)
{
    // First create through ThreadMan, then switch library records for lock/unlock.
    ServicePrxFixture program("ThreadManForUser", {0x19CFF145});
    program.fixture.word(0x170, 0xA8); // second import record
    program.fixture.word(0x194, 0x1A0);
    program.fixture.word(0x198, 0x40000000);
    program.fixture.word(0x19C, 0x00020005);
    program.fixture.word(0x1A0, 0x104);
    program.fixture.word(0x1A4, 0x128);
    program.fixture.name(0x2A0, "Kernel_Library");
    program.fixture.word(0x204, 0xBEA46419);
    program.fixture.word(0x208, 0x15B6446B);
    for (const auto offset : {0x94U, 0xA0U, 0xA4U})
    {
        program.fixture.relocation(offset, 2);
    }
    program.fixture.name(0x280, "mutex");
    program.argument(0, 0x08800200);
    program.argument(1, 0x08800180);
    program.argument(2, 0x200);
    program.argument(3, 0);
    program.argument(4, 0);
    program.call(0);
    program.argument(1, 2);
    program.argument(2, 0);
    program.call(1);
    program.argument(1, 1);
    program.call(2);
    program.instruction(0x8C820000); // remaining lock count
    EXPECT_EQ(execute_prx(program.finish()).exit_code, 1);
    program.offset -= 12;
    program.argument(1, 2); // only one lock remains
    program.call(2);
    EXPECT_THROW(execute_prx(program.finish()), std::runtime_error);
}

TEST(ExecutionTest, SemaphoreWaitsConsumeSignalsAndRejectBlocking)
{
    ServicePrxFixture program("ThreadManForUser", {0xD6DA4BA1, 0x4E3A1105, 0x3F53E640});
    program.fixture.name(0x280, "semaphore");
    program.argument(0, 0x08800180);
    program.argument(1, 0);
    program.argument(2, 1);
    program.argument(3, 1);
    program.argument(4, 0);
    program.call(0);
    program.instruction(0x00402021); // move $a0, $v0: semaphore ID
    program.argument(1, 1);
    program.argument(2, 0);
    program.call(1);
    const auto second_call = program.offset;
    program.call(2); // signal replenishes the consumed count
    program.call(1);
    EXPECT_EQ(execute_prx(program.finish()).exit_code, 0);
    program.fixture.word(second_call, 0x0C00004A); // wait again without a signal
    EXPECT_THROW(execute_prx(read_prx(program.fixture.bytes)), std::runtime_error);
}

TEST(ExecutionTest, SemaphoreSignalsRejectOverflowAndDeletedIdentifiers)
{
    ServicePrxFixture program("ThreadManForUser", {0xD6DA4BA1, 0x28B6489C, 0x3F53E640});
    program.fixture.name(0x280, "semaphore");
    program.argument(0, 0x08800180);
    program.argument(1, 0);
    program.argument(2, 1);
    program.argument(3, 1);
    program.argument(4, 0);
    program.call(0);
    program.instruction(0x00402021); // move $a0, $v0
    program.argument(1, 1);
    const auto delete_call = program.offset;
    program.call(1);
    EXPECT_EQ(execute_prx(program.finish()).exit_code, 0);
    program.offset -= 12;
    program.call(2);
    EXPECT_THROW(execute_prx(program.finish()), std::runtime_error);
    program.fixture.word(delete_call, 0x0C00004C); // signal the initially full semaphore
    EXPECT_THROW(execute_prx(read_prx(program.fixture.bytes)), std::runtime_error);
}

TEST(ExecutionTest, ThreadStatusReportsStackSizeAndRejectsUnsupportedLayouts)
{
    ServicePrxFixture program("ThreadManForUser", {0x17C1684E});
    program.argument(0, 0); // current thread
    program.argument(1, 0x08800200);
    program.argument(2, 104);
    const auto size_instruction = program.offset - 4;
    program.instruction(0xACA60000); // sw $a2, 0($a1): structure size
    program.call(0);
    program.instruction(0x8CA20034); // lw $v0, 52($a1): stack size
    EXPECT_EQ(execute_prx(program.finish()).exit_code, 0x10000);
    program.fixture.word(size_instruction, 0x34C60004); // requested structure size = 4
    EXPECT_THROW(execute_prx(read_prx(program.fixture.bytes)), std::runtime_error);
}

TEST(ExecutionTest, ReturnsGuestStatusAndCountsTheReturnDelaySlot)
{
    test::PrxFixture fixture;
    fixture.word(0x100, 0x03E00008); // jr $ra
    fixture.word(0x104, 0x24020007); // addiu $v0, $zero, 7 in the delay slot
    ExecutionOptions options;
    options.max_instructions = 2;
    const auto result = execute_prx(read_prx(fixture.bytes), options);
    EXPECT_EQ(result.exit_code, 7);
    EXPECT_TRUE(result.output.empty());
    EXPECT_EQ(result.instructions_executed, 2U);
}

TEST(ExecutionTest, CapturesGuestConsoleWritesAndRejectsInvalidBuffers)
{
    test::PrxFixture fixture;
    fixture.word(0x180, 0xD0);
    fixture.name(0x1D0, "IoFileMgrForUser");
    fixture.word(0x1B0, 0x42EC03AC); // sceIoWrite
    fixture.name(0x200, "hello");
    fixture.word(0x100, 0x27BDFFF0); // addiu $sp, $sp, -16
    fixture.word(0x104, 0xAFBF000C); // sw $ra, 12($sp)
    fixture.word(0x108, 0x24040001); // stdout
    fixture.word(0x10C, 0x3C050880); // lui $a1, 0x0880
    fixture.word(0x110, 0x34A50100); // ori $a1, $a1, 0x100
    fixture.word(0x114, 0x0C000030); // jal imported stub
    fixture.relocation(0x14, 4);
    fixture.word(0x118, 0x24060005); // length = 5 (delay slot)
    fixture.word(0x11C, 0x8FBF000C); // lw $ra, 12($sp)
    fixture.word(0x120, 0x03E00008); // jr $ra
    fixture.word(0x124, 0x27BD0010); // restore stack (delay slot)
    const auto result = execute_prx(read_prx(fixture.bytes));
    EXPECT_EQ(result.output, "hello");
    EXPECT_EQ(result.exit_code, 5);  // sceIoWrite returns the byte count in $v0.
    fixture.word(0x10C, 0x3C05FFFF); // Guest buffer exceeds mapped RAM.
    EXPECT_THROW(execute_prx(read_prx(fixture.bytes)), std::runtime_error);
}

TEST(ExecutionTest, PassesStartupArgumentsAsNulSeparatedBytes)
{
    test::PrxFixture fixture;
    fixture.word(0x180, 0xD0);
    fixture.name(0x1D0, "IoFileMgrForUser");
    fixture.word(0x1B0, 0x42EC03AC); // sceIoWrite
    fixture.word(0x100, 0x27BDFFF0); // addiu $sp, $sp, -16
    fixture.word(0x104, 0xAFBF000C); // sw $ra, 12($sp)
    fixture.word(0x108, 0x00803021); // addu $a2, $a0, $zero: argument block length
    fixture.word(0x10C, 0x24040001); // stdout; a1 already points to argument bytes
    fixture.word(0x110, 0x0C000030); // jal imported stub
    fixture.relocation(0x10, 4);
    fixture.word(0x114, 0);          // nop (delay slot)
    fixture.word(0x118, 0x8FBF000C); // lw $ra, 12($sp)
    fixture.word(0x11C, 0x03E00008); // jr $ra
    fixture.word(0x120, 0x27BD0010); // restore stack (delay slot)
    const auto parsed = read_prx(fixture.bytes);
    ExecutionOptions options;
    options.arguments = {"program.prx", "", "hello world"};
    const auto result = execute_prx(parsed, options);
    EXPECT_EQ(result.output, std::string("program.prx\0\0hello world\0", 25));
    EXPECT_EQ(result.exit_code, 25);
    options.arguments = {""};
    EXPECT_EQ(execute_prx(parsed, options).output, std::string(1, '\0'));
    options.arguments.clear();
    EXPECT_TRUE(execute_prx(parsed, options).output.empty());
}

TEST(ExecutionTest, EnforcesStartupArgumentStackCapacity)
{
    test::PrxFixture fixture;
    fixture.word(0x100, 0x03E00008); // jr $ra
    fixture.word(0x104, 0);          // nop (delay slot)
    const auto parsed = read_prx(fixture.bytes);
    ExecutionOptions options;
    options.arguments = {std::string(0x10000 - 257, 'x')};
    EXPECT_EQ(execute_prx(parsed, options).exit_code, 0);
    options.arguments.front().push_back('x');
    EXPECT_THROW(execute_prx(parsed, options), std::invalid_argument);
}

TEST(ExecutionTest, StopsAnInfiniteLoopAtTheInstructionBudget)
{
    test::PrxFixture fixture;
    fixture.word(0x100, 0x1000FFFF); // beq $zero, $zero, -1
    ExecutionOptions options;
    options.max_instructions = 10;
    try
    {
        execute_prx(read_prx(fixture.bytes), options);
        FAIL() << "An infinite loop must exhaust its budget";
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("Instruction budget exhausted"), std::string::npos);
        EXPECT_NE(message.find("0x8800000"), std::string::npos);
    }
}

TEST(ExecutionTest, ReportsCpuFaultsAndInvokedUnsupportedImports)
{
    test::PrxFixture fixture;
    fixture.word(0x100, 0xFFFFFFFF);
    try
    {
        execute_prx(read_prx(fixture.bytes));
        FAIL() << "Unsupported instructions must fail";
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("0x8800000"), std::string::npos);
        EXPECT_NE(message.find("0xffffffff"), std::string::npos);
    }
    fixture.word(0x100, 0x0C000030); // jal imported function at offset 0xC0
    fixture.relocation(0, 4);
    try
    {
        execute_prx(read_prx(fixture.bytes));
        FAIL() << "Invoking an unsupported import must fail";
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("TestLibrary:0x12345678"), std::string::npos);
        EXPECT_NE(message.find("0x88000c4"), std::string::npos);
    }
}

TEST(ExecutionTest, RejectsInvalidOptionsAndInsufficientRuntimeMemory)
{
    const test::PrxFixture fixture;
    const auto parsed = read_prx(fixture.bytes);
    ExecutionOptions options;
    options.max_instructions = 0;
    EXPECT_THROW(execute_prx(parsed, options), std::invalid_argument);
    options.max_instructions = 10;
    options.memory_size = 0x210; // The image fits; an aligned runtime allocation does not.
    EXPECT_THROW(execute_prx(parsed, options), std::runtime_error);
    options.memory_size = 0x1000; // The startup stack does not fit.
    EXPECT_THROW(execute_prx(parsed, options), std::runtime_error);
    options.memory_size = 0x01800000;
    options.arguments = {std::string("bad\0argument", 12)};
    EXPECT_THROW(execute_prx(parsed, options), std::invalid_argument);
}

} // namespace psp
