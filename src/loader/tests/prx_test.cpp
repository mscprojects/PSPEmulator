#include "cpu/cpu.hpp"
#include "loader/prx.hpp"
#include "loader/tests/prx_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace psp
{

using test::PrxFixture;

namespace
{

struct HeaderPatch
{
    std::size_t offset;
    std::uint32_t value;
};

constexpr std::uint32_t kLoadAddress = 0x08800000;
constexpr std::size_t kMemorySize = 0x1000;

} // namespace

TEST(PrxTest, PreparesIndependentImagesFromOneParsedPrx)
{
    const PrxFixture fixture;
    const Payload original_bytes = fixture.bytes;
    const auto parsed = read_prx(fixture.bytes);
    auto first = prepare_prx(parsed, GuestAddress{0x08800000}, 0x1000);
    const auto second = prepare_prx(parsed, GuestAddress{0x08900000}, 0x2000);
    EXPECT_EQ(first.module.global_pointer, GuestAddress{0x08800100});
    EXPECT_EQ(second.module.global_pointer, GuestAddress{0x08900100});
    EXPECT_EQ(first.memory.read_u32(GuestAddress{0x08800060}), 0x08800100U);
    EXPECT_EQ(second.memory.read_u32(GuestAddress{0x08900060}), 0x08900100U);
    first.memory.write_u32(GuestAddress{0x08800060}, 0);
    EXPECT_EQ(second.memory.read_u32(GuestAddress{0x08900060}), 0x08900100U);
    EXPECT_EQ(parsed.segments.front().virtual_address, 0U);
    EXPECT_EQ(fixture.bytes, original_bytes);
}

TEST(PrxTest, LoadedImageUsesThePspMemoryLayoutAndRejectsInvalidRamPlacement)
{
    const test::PrxFixture fixture;
    auto loaded = prepare_prx(read_prx(fixture.bytes), GuestAddress{0x08800000}, 0x1000);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{0x48800060}), 0x08800100U);
    loaded.memory.write_u32(GuestAddress{0x44000000}, 0xAABBCCDD);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{0x04000000}), 0xAABBCCDDU);
    EXPECT_EQ(loaded.image_end, GuestAddress{0x08800200});
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{0x48800000}, 0x1000), std::invalid_argument);
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{0x3FFFF000}, 0x2000), std::invalid_argument);
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{0x04000000}, 0x1000), std::invalid_argument);
}

TEST(PrxTest, PreparesAfterSourcePayloadIsDestroyed)
{
    const auto parsed = []
    {
        const PrxFixture fixture;
        return read_prx(fixture.bytes);
    }();
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        const auto loaded = prepare_prx(parsed, GuestAddress{base}, kMemorySize);
        EXPECT_EQ(loaded.memory.read_u32(loaded.entry_point), 0x24020007U);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x180}), 0U);
        EXPECT_EQ(loaded.module.name, "Fixture");
        EXPECT_EQ(loaded.module.global_pointer, GuestAddress{base + 0x100});
        ASSERT_EQ(loaded.imports.size(), 1U);
        EXPECT_EQ(loaded.imports.front().name, "TestLibrary");
    }
}

TEST(PrxTest, ChecksMemoryCapacityDuringPreparation)
{
    PrxFixture fixture;
    fixture.word(72, 0x2000);
    const auto parsed = read_prx(fixture.bytes);
    EXPECT_THROW(prepare_prx(parsed, GuestAddress{kLoadAddress}, 0x1000), std::invalid_argument);
    EXPECT_NO_THROW(prepare_prx(parsed, GuestAddress{kLoadAddress}, 0x2000));
}

TEST(PrxTest, LoadsSegmentsBssModuleAndUnresolvedImports)
{
    const PrxFixture fixture;
    auto loaded = prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, kMemorySize);
    EXPECT_EQ(loaded.entry_point, GuestAddress{kLoadAddress});
    EXPECT_EQ(loaded.memory.read_u32(loaded.entry_point), 0x24020007U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{kLoadAddress + 0x180}), 0U); // BSS
    EXPECT_EQ(loaded.memory.read_u8(GuestAddress{kLoadAddress + 0x1FF}), 0U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{kLoadAddress + 0x800}), 0U); // spare RAM
    EXPECT_THROW(loaded.memory.read_u8(GuestAddress{kLoadAddress + kMemorySize}), std::out_of_range);
    EXPECT_EQ(loaded.module.name, "Fixture");
    EXPECT_EQ(loaded.module.attributes, 2);
    EXPECT_EQ(loaded.module.version, 0x0102);
    EXPECT_EQ(loaded.module.global_pointer, GuestAddress{kLoadAddress + 0x100});
    ASSERT_EQ(loaded.imports.size(), 1U);
    const auto &library = loaded.imports.front();
    EXPECT_EQ(library.name, "TestLibrary");
    EXPECT_EQ(library.version, 0x0304);
    EXPECT_EQ(library.attributes, 0x4000);
    ASSERT_EQ(library.functions.size(), 1U);
    EXPECT_EQ(library.functions.front().nid, 0x12345678U);
    EXPECT_EQ(library.functions.front().stub_address, GuestAddress{kLoadAddress + 0xC0});
    EXPECT_EQ(loaded.memory.read_u32(library.functions.front().stub_address), 0x03E00008U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{kLoadAddress + 0xC4}), 0U);
    CpuState state{.program_counter = loaded.entry_point};
    Cpu cpu(loaded.memory);
    cpu.step(state);
    EXPECT_EQ(state.registers[2], 7U);
}

TEST(PrxTest, RelocatesWordsJumpsAndSignedHighLowPairs)
{
    PrxFixture fixture;
    fixture.word(0x110, 0x3C080001); // lui $t0, 1
    fixture.word(0x114, 0x25088000); // addiu $t0, $t0, -32768
    fixture.word(0x118, 0x3C090002); // second HI16 sharing the same LO16
    fixture.word(0x11C, 0x2788FFF0); // GP-relative instruction remains unchanged
    fixture.word(0x120, 0x24081234);
    fixture.word(0x124, 0x0C000010); // jal relative address 0x40
    fixture.word(0x128, 0x12345678);
    fixture.relocation(0x10, 5);
    fixture.relocation(0x18, 5);
    fixture.relocation(0x14, 6);
    fixture.relocation(0x1C, 7);
    fixture.relocation(0x20, 1);
    fixture.relocation(0x24, 4);
    fixture.relocation(0x28, 0); // NONE
    const std::uint32_t base = 0x08801234;
    const auto loaded = prepare_prx(read_prx(fixture.bytes), GuestAddress{base}, kMemorySize);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x10}), 0x3C080881U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x14}), 0x25089234U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x18}), 0x3C090882U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x1C}), 0x2788FFF0U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x20}), 0x24082468U);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x24}), 0x0E20049DU);
    EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x28}), 0x12345678U);
}

TEST(PrxTest, RelocatesAcrossMultipleSegmentsAndLeavesTheirGapZeroed)
{
    PrxFixture fixture;
    fixture.halfword(44, 2);
    fixture.word(84, 1);     // second PT_LOAD
    fixture.word(88, 0x500); // file offset
    fixture.word(92, 0x300); // relative virtual address
    fixture.word(100, 0x40); // file size
    fixture.word(104, 0x80); // memory size
    fixture.word(108, 6);    // PF_R | PF_W
    fixture.word(112, 4);
    fixture.word(0x110, 0x20);
    fixture.word(0x500, 0x18);
    fixture.relocation(0x10, 0x00010002); // patch segment 0, reference segment 1
    fixture.relocation(0, 0x00000102);    // patch segment 1, reference segment 0
    const auto parsed = read_prx(fixture.bytes);
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        const auto loaded = prepare_prx(parsed, GuestAddress{base}, kMemorySize);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x10}), base + 0x320);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x300}), base + 0x18);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x280}), 0U);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x340}), 0U);
    }
}

TEST(PrxTest, RejectsOverlappingSegmentsAndMisalignedLoadBases)
{
    PrxFixture overlap;
    overlap.halfword(44, 2);
    overlap.word(84, 1);     // second PT_LOAD
    overlap.word(88, 0x500); // valid source range
    overlap.word(92, 0x100); // overlaps the first segment's [0, 0x200) guest range
    overlap.word(100, 0x40);
    overlap.word(104, 0x80);
    overlap.word(108, 6);
    overlap.word(112, 4);
    EXPECT_THROW(prepare_prx(read_prx(overlap.bytes), GuestAddress{kLoadAddress}, kMemorySize), std::invalid_argument);

    PrxFixture aligned;
    aligned.word(80, 0x100); // file offset and relative address satisfy this alignment
    EXPECT_THROW(prepare_prx(read_prx(aligned.bytes), GuestAddress{kLoadAddress + 4}, kMemorySize),
                 std::invalid_argument);
    EXPECT_NO_THROW(prepare_prx(read_prx(aligned.bytes), GuestAddress{kLoadAddress + 0x100}, kMemorySize));
}

TEST(PrxTest, LoadsRelocationsFromProgramHeadersWithoutSections)
{
    PrxFixture fixture;
    fixture.word(32, 0);
    fixture.halfword(46, 0);
    fixture.halfword(48, 0);
    fixture.halfword(44, 2);
    fixture.word(84, 0x700000A0);
    fixture.word(88, 0x300);
    fixture.word(100, fixture.relocation_count * 8);
    const auto loaded = prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, kMemorySize);
    EXPECT_EQ(loaded.module.global_pointer, GuestAddress{kLoadAddress + 0x100});
    ASSERT_EQ(loaded.imports.size(), 1U);
    EXPECT_EQ(loaded.imports.front().name, "TestLibrary");
}

TEST(PrxTest, RejectsInvalidSegmentsMemoryAndEntryPoints)
{
    const HeaderPatch patches[] = {
        {24, 1},          // unaligned entry
        {24, 0x800},      // entry outside the image
        {52, 0},          // no first load segment
        {56, 0xFFFFFFFC}, // file range beyond EOF
        {60, 0xFFFFFFFC}, // segment virtual address beyond RAM
        {64, 0x80},       // module info before the segment
        {68, 0x201},      // file size larger than memory size
        {72, 0x1001},     // image larger than supplied RAM
        {76, 4},          // entry segment is not executable
        {80, 3},          // invalid alignment
    };
    for (const auto &patch : patches)
    {
        SCOPED_TRACE(patch.offset);
        PrxFixture fixture;
        fixture.word(patch.offset, patch.value);
        EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                     std::invalid_argument);
    }
    const PrxFixture fixture;
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress + 1}, kMemorySize),
                 std::invalid_argument);
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{0xFFFFFFFC}, kMemorySize), std::invalid_argument);
    EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, 0), std::invalid_argument);
}

TEST(PrxTest, RejectsInvalidOrUnsupportedRelocations)
{
    for (const std::uint32_t info : {8U, 0x00000102U, 0x00010002U, 0x01000002U})
    {
        SCOPED_TRACE(info);
        PrxFixture fixture;
        fixture.relocation(0x10, info);
        EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                     std::invalid_argument);
    }
    for (const std::uint32_t offset : {0x1FFU, 0xFFFFFFFCU})
    {
        PrxFixture fixture;
        fixture.relocation(offset, 2);
        EXPECT_THROW(prepare_prx(read_prx(fixture.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                     std::invalid_argument);
    }
    PrxFixture missing_low;
    missing_low.relocation(0x10, 5);
    EXPECT_THROW(prepare_prx(read_prx(missing_low.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture misaligned;
    misaligned.relocation(1, 4);
    EXPECT_THROW(prepare_prx(read_prx(misaligned.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture truncated;
    truncated.word(0x664, truncated.relocation_count * 8 + 1);
    EXPECT_THROW(prepare_prx(read_prx(truncated.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture compressed;
    compressed.word(0x654, 0x700000A1);
    EXPECT_THROW(prepare_prx(read_prx(compressed.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
}

TEST(PrxTest, AllowsGlobalPointerOutsideMemoryForSignedSmallDataAccess)
{
    PrxFixture fixture;
    fixture.word(0x160, 0x8020);     // GP base, outside the segment and guest memory
    fixture.word(0x100, 0x8F828000); // lw $v0, -32768($gp)
    fixture.word(0x120, 0x12345678); // small data at GP - 32768
    const auto parsed = read_prx(fixture.bytes);
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        auto loaded = prepare_prx(parsed, GuestAddress{base}, kMemorySize);
        EXPECT_EQ(loaded.module.global_pointer, GuestAddress{base + 0x8020});
        EXPECT_THROW(loaded.memory.read_u8(loaded.module.global_pointer), std::out_of_range);
        CpuState state{.program_counter = loaded.entry_point};
        state.registers[28] = loaded.module.global_pointer.value_of();
        Cpu cpu(loaded.memory);
        cpu.step(state);
        EXPECT_EQ(state.registers[2], 0x12345678U);
    }
}

TEST(PrxTest, RejectsMalformedModuleAndImportPointers)
{
    PrxFixture reversed;
    reversed.word(0x170, 0x7C);
    EXPECT_THROW(prepare_prx(read_prx(reversed.bytes), GuestAddress{kLoadAddress}, kMemorySize), std::invalid_argument);
    PrxFixture bad_name;
    bad_name.word(0x180, 0x300);
    EXPECT_THROW(prepare_prx(read_prx(bad_name.bytes), GuestAddress{kLoadAddress}, kMemorySize), std::invalid_argument);
    PrxFixture bad_stubs;
    bad_stubs.word(0x190, 0x1FC);
    EXPECT_THROW(prepare_prx(read_prx(bad_stubs.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture too_many_functions;
    too_many_functions.halfword(0x18A, 0xFFFF);
    EXPECT_THROW(prepare_prx(read_prx(too_many_functions.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture zero_length;
    zero_length.bytes[0x188] = 0;
    EXPECT_THROW(prepare_prx(read_prx(zero_length.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture variables;
    variables.bytes[0x189] = 1;
    EXPECT_THROW(prepare_prx(read_prx(variables.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
    PrxFixture unterminated;
    unterminated.word(72, 0x180);
    unterminated.word(0x180, 0x17C);
    unterminated.word(0x27C, 0x41414141);
    EXPECT_THROW(prepare_prx(read_prx(unterminated.bytes), GuestAddress{kLoadAddress}, kMemorySize),
                 std::invalid_argument);
}

TEST(PrxTest, LoadsBundledLsuWithGlobalPointerBeyondItsImage)
{
    std::ifstream input(std::string(PSPAUTOTESTS_ROOT) + "/tests/cpu/lsu/lsu.prx", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const Payload bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto parsed = read_prx(bytes);
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        // Enough RAM for the image, but not for the biased GP base itself.
        const auto loaded = prepare_prx(parsed, GuestAddress{base}, 0x1E570);
        EXPECT_EQ(loaded.entry_point, GuestAddress{base + 0xAC});
        EXPECT_EQ(loaded.module.name, "TESTMODULE");
        EXPECT_EQ(loaded.module.global_pointer, GuestAddress{base + 0x20040});
        EXPECT_EQ(loaded.module.exports_begin, GuestAddress{base + 0x15828});
        EXPECT_EQ(loaded.module.exports_end, GuestAddress{base + 0x15838});
        ASSERT_EQ(loaded.imports.size(), 9U);
        const auto &library = loaded.imports.front();
        EXPECT_EQ(library.name, "SysMemUserForUser");
        ASSERT_EQ(library.functions.size(), 29U);
        EXPECT_EQ(library.functions.front().nid, 0xA291F107U);
        EXPECT_EQ(library.functions.front().stub_address, GuestAddress{base + 0x15624});
        EXPECT_EQ(loaded.memory.read_u32(library.functions.front().stub_address), 0x03E00008U);
    }
}

TEST(PrxTest, LoadsBundledCpuAluAtDifferentAddresses)
{
    std::ifstream input(std::string(PSPAUTOTESTS_ROOT) + "/tests/cpu/cpu_alu/cpu_alu.prx", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const Payload bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto parsed = read_prx(bytes);
    for (const std::uint32_t base : {0x08800000U, 0x08900000U})
    {
        SCOPED_TRACE(base);
        const auto loaded = prepare_prx(parsed, GuestAddress{base}, 0x100000);
        EXPECT_EQ(loaded.entry_point, GuestAddress{base + 0xAC});
        EXPECT_EQ(loaded.module.name, "TESTMODULE");
        EXPECT_EQ(loaded.module.global_pointer, GuestAddress{base + 0x27050});
        EXPECT_EQ(loaded.module.exports_begin, GuestAddress{base + 0x1C800});
        EXPECT_EQ(loaded.module.exports_end, GuestAddress{base + 0x1C810});
        ASSERT_EQ(loaded.imports.size(), 9U);
        EXPECT_EQ(loaded.imports.front().name, "SysMemUserForUser");
        ASSERT_EQ(loaded.imports.front().functions.size(), 29U);
        EXPECT_EQ(loaded.imports.front().functions.front().nid, 0xA291F107U);
        EXPECT_EQ(loaded.imports.front().functions.front().stub_address, GuestAddress{base + 0x1C5E4});
        std::size_t functions = 0;
        for (const auto &library : loaded.imports)
        {
            functions += library.functions.size();
        }
        EXPECT_EQ(functions, 67U);
        EXPECT_EQ(loaded.memory.read_u32(GuestAddress{base + 0x1F068}), 0U);
        EXPECT_EQ(loaded.memory.read_u8(GuestAddress{base + 0x3558B}), 0U);
    }
}

} // namespace psp
