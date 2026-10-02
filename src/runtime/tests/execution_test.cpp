#include "runtime/execution.hpp"

#include "loader/tests/prx_fixture.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace psp
{

TEST(ExecutionTest, ExecutesBundledCpuAluAndMatchesEntireHardwareOutput)
{
    const std::string directory = std::string(PSPAUTOTESTS_ROOT) + "/tests/cpu/cpu_alu/";
    std::ifstream input(directory + "cpu_alu.prx", std::ios::binary);
    std::ifstream expected_file(directory + "cpu_alu.expected", std::ios::binary);
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
