#include "cpu/cpu.hpp"
#include "runtime/ge.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <limits>
#include <string>
#include <vector>

namespace psp::detail
{

namespace
{

class GeTest : public testing::Test
{
protected:
    GeTest()
        : memory(GuestAddress{0x08000000}, 0x40000),
          kernel(memory, GuestAddress{0x08000000}, 0x40000, {}, GuestAddress{0x08000100}), ge(memory, kernel, 1000)
    {
        memory.map_region(GuestAddress{0x04000000}, 0x200000);
        memory.map_alias(GuestAddress{0x44000000}, GuestAddress{0x04000000});
        kernel.initialize(GuestAddress{0x08000400}, {});
    }

    std::uint32_t submit(const std::vector<std::uint32_t> &words, std::uint32_t address = 0x08001000,
                         std::uint32_t stall = 0, std::uint32_t callback = 0xFFFFFFFF)
    {
        for (std::size_t index = 0; index < words.size(); ++index)
            memory.write_u32(GuestAddress{address + static_cast<std::uint32_t>(index * 4)}, words[index]);
        return ge.enqueue({GuestAddress{address}, GuestAddress{stall}, callback, GuestAddress{0}});
    }

    void drain()
    {
        for (unsigned count = 0; ge.runnable() && count < 1000; ++count)
            ge.step();
        ASSERT_FALSE(ge.runnable());
    }

    std::vector<std::uint32_t> drawing(bool floating = false)
    {
        return {0x10080000, 0x01002000, floating ? 0x1280019CU : 0x1280011CU,
                0xD2000003, 0x9C000000, 0x9D000200,
                0x16043DDF, 0xD5043DDF, 0xE7000001,
                0x50000001};
    }

    // Vertex slot, coordinates, and packed color are separate fixture fields.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void vertex(unsigned index, unsigned x, unsigned y, std::uint32_t color)
    {
        const auto address = 0x08002000 + index * 12;
        memory.write_u32(GuestAddress{address}, color);
        memory.write_u16(GuestAddress{address + 4}, static_cast<std::uint16_t>(x));
        memory.write_u16(GuestAddress{address + 6}, static_cast<std::uint16_t>(y));
    }

    std::uint32_t pixel(unsigned x, unsigned y, std::uint32_t offset = 0)
    {
        return memory.read_u32(GuestAddress{0x04000000 + offset + (y * 512 + x) * 4});
    }

    std::uint32_t callback()
    {
        memory.write_u32(GuestAddress{0x08003008}, 0x08000400);
        memory.write_u32(GuestAddress{0x0800300C}, 0x08003200);
        return ge.set_callback(GuestAddress{0x08003000});
    }

    Memory memory;
    Kernel kernel;
    Ge ge;
};

TEST_F(GeTest, StalledFifoListsResumeAtUpdatedAddressAndReuseCompletedSlots)
{
    const auto first = submit({0, 0x0F000000, 0x0C000000}, 0x08001000, 0x08001000);
    const auto second = submit({0x0F000000, 0x0C000000}, 0x08001100);
    EXPECT_FALSE(ge.runnable());
    EXPECT_EQ(ge.list_sync(first, 1), 3U);
    EXPECT_EQ(ge.list_sync(second, 1), 1U);
    ge.step();
    EXPECT_EQ(ge.draw_sync(1), 3U);
    ge.update_stall(first, GuestAddress{0x48001004});
    EXPECT_EQ(ge.list_sync(first, 1), 2U);
    ge.step();
    EXPECT_EQ(ge.list_sync(first, 1), 3U);
    ge.update_stall(first, GuestAddress{0});
    ge.step(); // FINISH is not END.
    EXPECT_EQ(ge.list_sync(first, 1), 2U);
    ge.step();
    EXPECT_EQ(ge.list_sync(first, 1), 0U);
    EXPECT_EQ(ge.list_sync(second, 1), 2U);
    drain();
    EXPECT_EQ(ge.draw_sync(0), 0U);
    EXPECT_EQ(submit({0x0F000000, 0x0C000000}), first);
    EXPECT_THROW(ge.update_stall(second, GuestAddress{0}), std::runtime_error);
}

TEST_F(GeTest, ListSyncWakesItsThreadOnlyAtEndAndDrawSyncWaitsForEveryList)
{
    const auto first = submit({0x0F000000, 0x0C000000});
    submit({0x0F000000, 0x0C000000}, 0x08001100, 0x08001100);
    const auto main = kernel.current_thread_id();
    EXPECT_FALSE(ge.list_sync(first, 0));
    EXPECT_EQ(kernel.select_next_thread(0), ThreadSelection::Idle);
    ge.step();
    EXPECT_EQ(kernel.select_next_thread(0), ThreadSelection::Idle);
    ge.step();
    ASSERT_EQ(kernel.select_next_thread(0), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_id(), main);
    EXPECT_EQ(kernel.current_thread_state().registers[2], 0U);
    EXPECT_FALSE(ge.draw_sync(0));
    EXPECT_EQ(kernel.select_next_thread(0), ThreadSelection::Idle);
    ge.update_stall(1, GuestAddress{0});
    drain();
    EXPECT_EQ(kernel.select_next_thread(0), ThreadSelection::Ready);
    EXPECT_EQ(kernel.current_thread_state().registers[2], 0U);
}

TEST_F(GeTest, FinishCallbackExecutesGuestCodeAndDefersMaskedAndNestedDelivery)
{
    const auto cb = callback();
    // Write ABI arguments, clobber integer/FPU state, and return nonzero.
    constexpr std::array code{0xACA40000U, 0xACA50004U, 0x00800011U, 0x00800013U, 0x24100063U,
                              0x44840800U, 0x44C0F800U, 0x24020009U, 0x03E00008U, 0U};
    for (std::size_t index = 0; index < code.size(); ++index)
        memory.write_u32(GuestAddress{0x08000400 + static_cast<std::uint32_t>(index * 4)}, code[index]);
    auto &state = kernel.current_thread_state();
    state.program_counter = GuestAddress{0x08000500};
    state.next_program_counter = GuestAddress{0x08000900}; // Preserve a pending branch target.
    state.high_register = 0x12345678;
    state.low_register = 0xABCDEF01;
    state.floating_point_registers.fill(0x1234);
    state.floating_point_control = 0x01800E03;
    state.floating_point_branch_condition = true;
    state.load_linked = true;
    auto saved = state;
    saved.load_linked = false;
    const auto stack = memory.read_bytes(GuestAddress{state.registers[29]}, 64);
    const auto enabled = kernel.suspend_interrupts();
    submit({0x0F000007, 0x0C000000, 0}, 0x08001000, 0, cb);
    submit({0x0F00000B, 0x0C000000}, 0x08001100, 0, cb);
    drain();
    ge.deliver_interrupt();
    EXPECT_EQ(state.program_counter, saved.program_counter);
    kernel.resume_interrupts(enabled);
    Cpu cpu(memory);
    for (const auto argument : {7U, 11U})
    {
        ge.deliver_interrupt();
        EXPECT_EQ(state.registers[4], argument);
        EXPECT_EQ(state.registers[5], 0x08003200U);
        const auto entry = state;
        ge.deliver_interrupt();
        EXPECT_EQ(state, entry);
        for (std::size_t index = 0; index < code.size(); ++index)
            EXPECT_FALSE(cpu.step(state));
        EXPECT_EQ(memory.read_u32(GuestAddress{0x08003200}), argument);
        EXPECT_EQ(memory.read_u32(GuestAddress{0x08003204}), 0x08003200U);
        EXPECT_EQ(kernel.select_next_thread(0), ThreadSelection::Ready);
        EXPECT_EQ(state, saved); // Includes ignored nonzero callback return.
        EXPECT_EQ(memory.read_bytes(GuestAddress{state.registers[29]}, 64), stack);
    }
}

TEST_F(GeTest, ValidatesSubmissionCallbackPointersModesAndSlotCapacity)
{
    EXPECT_THROW(Ge(memory, kernel, 0), std::invalid_argument);
    EXPECT_THROW(ge.enqueue({GuestAddress{0}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}}), std::runtime_error);
    EXPECT_THROW(ge.enqueue({GuestAddress{0x08001001}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}}),
                 std::runtime_error);
    EXPECT_THROW(ge.enqueue({GuestAddress{0x08001000}, GuestAddress{0x08001002}, 0xFFFFFFFF, GuestAddress{0}}),
                 std::runtime_error);
    EXPECT_THROW(ge.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{4}}),
                 std::runtime_error);
    EXPECT_THROW(ge.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0, GuestAddress{0}}), std::runtime_error);
    EXPECT_THROW(ge.list_sync(0, 1), std::runtime_error);
    EXPECT_THROW(ge.draw_sync(2), std::runtime_error);
    EXPECT_THROW(ge.set_callback(GuestAddress{0x0803FFFC}), std::out_of_range);
    memory.write_u32(GuestAddress{0x08003008}, 0x08000401);
    EXPECT_THROW(ge.set_callback(GuestAddress{0x08003000}), std::runtime_error);
    for (unsigned index = 0; index < 16; ++index)
        EXPECT_EQ(callback(), index);
    EXPECT_THROW(callback(), std::runtime_error);
    ge.unset_callback(3);
    EXPECT_THROW(ge.unset_callback(3), std::runtime_error);
    EXPECT_EQ(callback(), 3U);
    for (unsigned index = 0; index < 64; ++index)
        EXPECT_EQ(submit({0x0F000000, 0x0C000000}, 0x08001000, 0x08001000), index);
    EXPECT_THROW(submit({0}), std::runtime_error);
}

TEST_F(GeTest, JumpSkipsInlineDataAndDiagnosticsIdentifyBadCommandAndAddress)
{
    const auto id = submit({0x10080000, 0x08001010, 0xFFFFFFFF, 0xFFFFFFFF, 0x0F000000, 0x0C000000});
    drain();
    EXPECT_EQ(ge.list_sync(id, 1), 0U);
    for (const auto word : {0xFF123456U, 0x0C000000U, 0x0E000001U, 0xC4000001U})
    {
        Ge isolated(memory, kernel, 10);
        memory.write_u32(GuestAddress{0x08001000}, word);
        isolated.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
        try
        {
            isolated.step();
            FAIL() << "expected invalid list";
        }
        catch (const std::runtime_error &error)
        {
            const std::string message(error.what());
            EXPECT_NE(message.find("at 0x8001000 (command 0x"), std::string::npos);
            if (word == 0xFF123456)
                EXPECT_NE(message.find("command 0xff123456"), std::string::npos);
        }
    }
}

TEST_F(GeTest, CyclicJumpIsBoundedEvenWithAllCpuThreadsWaiting)
{
    Ge bounded(memory, kernel, 3);
    memory.write_u32(GuestAddress{0x08001000}, 0x10080000);
    memory.write_u32(GuestAddress{0x08001004}, 0x08001004);
    bounded.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
    kernel.wait_ge();
    for (unsigned index = 0; index < 3; ++index)
    {
        EXPECT_EQ(kernel.select_next_thread(kernel.system_time()), ThreadSelection::Idle);
        bounded.step();
        kernel.advance_time(1);
    }
    EXPECT_THROW(bounded.step(), std::runtime_error);
    EXPECT_EQ(kernel.system_time(), 3U);
}

TEST_F(GeTest, SmoothTriangleUsesStrideAndRgbaOrderAndReversedWindingMatches)
{
    vertex(0, 0, 0, 0xFF0000FF);
    vertex(1, 4, 0, 0xFF00FF00);
    vertex(2, 0, 4, 0xFFFF0000);
    auto words = drawing();
    words.insert(words.end(), {0x04030003, 0x0F000000, 0x0C000000});
    submit(words);
    drain();
    EXPECT_EQ(pixel(0, 0), 0xFF1F1FBFU); // weights 3/4,1/8,1/8
    EXPECT_EQ(pixel(1, 1), 0xFF5F5F3FU);
    EXPECT_EQ(pixel(3, 0), 0U);   // Diagonal belongs to the adjoining triangle.
    EXPECT_EQ(pixel(480, 0), 0U); // Row padding is untouched.
    const auto original = memory.read_bytes(GuestAddress{0x04000000}, std::size_t{512} * 272 * 4);
    vertex(1, 0, 4, 0xFFFF0000);
    vertex(2, 4, 0, 0xFF00FF00);
    memory.write_bytes(GuestAddress{0x04000000}, Payload(original.size()));
    submit(words);
    drain();
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
}

TEST_F(GeTest, FlatTriangleUsesLastVertexAndDegenerateTriangleDoesNotWrite)
{
    vertex(0, 0, 0, 1);
    vertex(1, 0, 4, 2);
    vertex(2, 4, 0, 0xAABBCCDD);
    auto words = drawing();
    words.insert(words.end(), {0x50000000, 0x04030003, 0x0F000000, 0x0C000000});
    submit(words);
    drain();
    EXPECT_EQ(pixel(0, 0), 0xAABBCCDDU);
    const auto original = memory.read_bytes(GuestAddress{0x04000000}, std::size_t{512} * 272 * 4);
    vertex(2, 0, 8, 3);
    submit(words);
    drain();
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
}

TEST_F(GeTest, TwoTrianglesShareAnEdgeWithoutHolesOrDoubleOwnership)
{
    auto words = drawing();
    words.insert(words.end(), {0x50000000, 0x04030003, 0x04030003, 0x0F000000, 0x0C000000});
    vertex(0, 0, 0, 1);
    vertex(1, 4, 0, 1);
    vertex(2, 0, 4, 1);
    vertex(3, 4, 0, 2);
    vertex(4, 4, 4, 2);
    vertex(5, 0, 4, 2);
    submit(words);
    drain();
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 5; ++x)
            EXPECT_EQ(pixel(x, y), x >= 4 || y >= 4 ? 0U : x + y < 3 ? 1U : 2U);
    const auto original = memory.read_bytes(GuestAddress{0x04000000}, std::size_t{512} * 272 * 4);
    // Reverse primitive submission order; double ownership would change edge pixels.
    const auto first = memory.read_bytes(GuestAddress{0x08002000}, 36);
    const auto second = memory.read_bytes(GuestAddress{0x08002024}, 36);
    memory.write_bytes(GuestAddress{0x08002000}, second);
    memory.write_bytes(GuestAddress{0x08002024}, first);
    submit(words);
    drain();
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
}

TEST_F(GeTest, ClearClipsToRegionAndScissorAndRgbPreservesAlpha)
{
    vertex(0, 0, 0, 0);
    vertex(1, 480, 272, 0x12345678);
    for (const auto alpha : {false, true})
    {
        memory.write_u32(GuestAddress{0x04010000 + (2 * 512 + 2) * 4}, 0xAA000000);
        auto words = drawing();
        words.insert(words.end(), {0x9C410000, 0x9D040200, // Mirrored VRAM address plus offset.
                                   0x15000801, 0xD4000402, 0xD5000C03, alpha ? 0xD3000301U : 0xD3000101U, 0x04060002,
                                   0x0F000000, 0x0C000000});
        submit(words);
        drain();
        EXPECT_EQ(pixel(2, 2, 0x10000), alpha ? 0x12345678U : 0xAA345678U);
        EXPECT_EQ(pixel(1, 2, 0x10000), 0U);
        EXPECT_EQ(pixel(2, 1, 0x10000), 0U);
        EXPECT_EQ(pixel(4, 2, 0x10000), 0U);
        EXPECT_EQ(memory.read_u32(GuestAddress{0x44010000 + (2 * 512 + 2) * 4}), pixel(2, 2, 0x10000));
    }
}

TEST_F(GeTest, UnsupportedDrawingStateFailsBeforeWritingVram)
{
    vertex(0, 0, 0, 1);
    vertex(1, 4, 0, 2);
    vertex(2, 0, 4, 3);
    const auto original = memory.read_bytes(GuestAddress{0x04000000}, 0x200000);
    for (const auto bad : {0x1200011CU, 0x1280091CU, 0x12800114U, 0xD2000002U, 0xE8000001U, 0xE9000001U, 0xE7000000U,
                           0x50000002U, 0x04030006U, 0x04040003U, 0x9D000004U, 0x9C1F0000U, 0x0103FFF0U, 0xD3000101U})
    {
        Ge isolated(memory, kernel, 100);
        auto words = drawing();
        words.push_back(bad);
        words.push_back(0x04030003);
        for (std::size_t index = 0; index < words.size(); ++index)
            memory.write_u32(GuestAddress{0x08001000 + static_cast<std::uint32_t>(index * 4)}, words[index]);
        isolated.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
        EXPECT_THROW(while (isolated.runnable()) isolated.step(), std::exception) << std::hex << bad;
        EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
    }
    for (const auto command : {0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU, 0x1DU, 0x1EU, 0x1FU, 0x20U, 0x21U, 0x22U, 0x23U,
                               0x24U, 0x25U, 0x26U, 0x27U, 0x28U})
    {
        Ge isolated(memory, kernel, 100);
        auto words = drawing();
        words.insert(words.end(), {(command << 24) | 1, 0x04030003});
        for (std::size_t index = 0; index < words.size(); ++index)
            memory.write_u32(GuestAddress{0x08001000 + static_cast<std::uint32_t>(index * 4)}, words[index]);
        isolated.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
        EXPECT_THROW(while (isolated.runnable()) isolated.step(), std::runtime_error);
        EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
    }
}

TEST_F(GeTest, FloatingTrianglesSupportNegativeAndFractionalCoordinatesButClearsRequireIntegers)
{
    constexpr std::array<float, 3> xs{-0.5F, 3.5F, -0.5F};
    constexpr std::array<float, 3> ys{-0.5F, -0.5F, 3.5F};
    constexpr std::array colors{0xFF0000FFU, 0xFF00FF00U, 0xFFFF0000U};
    for (unsigned index = 0; index < 3; ++index)
    {
        memory.write_u32(GuestAddress{0x08002000 + index * 16}, colors[index]);
        memory.write_u32(GuestAddress{0x08002004 + index * 16}, std::bit_cast<std::uint32_t>(xs[index]));
        memory.write_u32(GuestAddress{0x08002008 + index * 16}, std::bit_cast<std::uint32_t>(ys[index]));
    }
    auto words = drawing(true);
    words.insert(words.end(), {0x04030003, 0x0F000000, 0x0C000000});
    submit(words);
    drain();
    EXPECT_EQ(pixel(0, 0), 0xFF3F3F7FU);
    EXPECT_EQ(pixel(2, 0), 0U);
    const auto original = memory.read_bytes(GuestAddress{0x04000000}, std::size_t{512} * 272 * 4);
    words = drawing(true);
    words.insert(words.end(), {0xD3000101, 0x04060002});
    submit(words);
    EXPECT_THROW(drain(), std::runtime_error);
    EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, original.size()), original);
}

TEST_F(GeTest, ListFetchAndJumpValidateMappedMemoryAndReportFaultingPc)
{
    for (const auto jump : {false, true})
    {
        Ge isolated(memory, kernel, 10);
        const auto address = jump ? 0x08001000U : 0x0803FFFCU;
        memory.write_u32(GuestAddress{address}, jump ? 0x080FFFFCU : 0);
        isolated.enqueue({GuestAddress{address}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
        try
        {
            isolated.step();
            isolated.step();
            FAIL() << "Expected unmapped GE memory fault";
        }
        catch (const std::runtime_error &error)
        {
            const std::string message(error.what());
            EXPECT_NE(message.find(jump ? "at 0x8001000 (command 0x080ffffc)" : "at 0x8040000"), std::string::npos);
        }
    }
}

TEST_F(GeTest, InvalidFloatCoordinatesFailBeforeAnyPixelWrite)
{
    for (const auto invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 65536.0F})
    {
        Ge isolated(memory, kernel, 100);
        for (unsigned index = 0; index < 3; ++index)
        {
            memory.write_u32(GuestAddress{0x08002000 + index * 16}, 0xFFFFFFFF);
            memory.write_u32(GuestAddress{0x08002004 + index * 16},
                             std::bit_cast<std::uint32_t>(index == 2 ? invalid : 4.0F));
            memory.write_u32(GuestAddress{0x08002008 + index * 16}, std::bit_cast<std::uint32_t>(4.0F));
        }
        auto words = drawing(true);
        words.push_back(0x04030003);
        for (std::size_t index = 0; index < words.size(); ++index)
            memory.write_u32(GuestAddress{0x08001000 + static_cast<std::uint32_t>(index * 4)}, words[index]);
        isolated.enqueue({GuestAddress{0x08001000}, GuestAddress{0}, 0xFFFFFFFF, GuestAddress{0}});
        EXPECT_THROW(while (isolated.runnable()) isolated.step(), std::runtime_error);
        EXPECT_EQ(memory.read_bytes(GuestAddress{0x04000000}, std::size_t{512} * 272 * 4),
                  Payload(std::size_t{512} * 272 * 4));
    }
}

} // namespace

} // namespace psp::detail
