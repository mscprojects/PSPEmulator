#include "loader/prx_reader.hpp"
#include "loader/tests/prx_fixture.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

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

} // namespace

TEST(PrxReaderTest, ReadsRelativeHeadersAndDecodedRelocations)
{
    const PrxFixture fixture;
    const auto parsed = read_prx(fixture.bytes);
    EXPECT_EQ(parsed.entry_offset, 0U);
    EXPECT_EQ(parsed.module_offset, 0x40U);
    const Payload contents(fixture.bytes.begin() + 0x100, fixture.bytes.begin() + 0x280);
    EXPECT_EQ(parsed.segments, (std::vector<PrxSegment>{{1, 0, 0x200, 5, 4, contents}}));
    ASSERT_EQ(parsed.relocation_tables.size(), 1U);
    EXPECT_EQ(
        parsed.relocation_tables.front().relocations,
        (std::vector<PrxRelocation>{
            {0x60, 2, 0, 0}, {0x6C, 2, 0, 0}, {0x70, 2, 0, 0}, {0x80, 2, 0, 0}, {0x8C, 2, 0, 0}, {0x90, 2, 0, 0}}));
}

TEST(PrxReaderTest, NormalizesKernelModuleOffsetAndRejectsUninitializedModuleInformation)
{
    PrxFixture fixture;
    fixture.word(64, 0x80000140); // Kernel flag does not form part of the file offset.
    EXPECT_EQ(read_prx(fixture.bytes).module_offset, 0x40U);
    for (const std::uint32_t offset : {0x80U, 0x250U, 0x280U, 0x300U})
    {
        SCOPED_TRACE(offset);
        fixture.word(64, offset);
        EXPECT_THROW(read_prx(fixture.bytes), std::invalid_argument);
    }
}

TEST(PrxReaderTest, DecodesCrossSegmentIndicesWithoutChoosingAddresses)
{
    PrxFixture fixture;
    fixture.halfword(44, 2);
    fixture.word(84, 1);
    fixture.word(88, 0x500);
    fixture.word(92, 0x300);
    fixture.word(100, 0x40);
    fixture.word(104, 0x80);
    fixture.word(108, 6);
    fixture.word(112, 4);
    fixture.relocation(0x10, 0x00010002);
    fixture.relocation(0, 0x00000102);
    const auto parsed = read_prx(fixture.bytes);
    ASSERT_EQ(parsed.segments.size(), 2U);
    EXPECT_EQ(parsed.segments[1].virtual_address, 0x300U);
    ASSERT_EQ(parsed.relocation_tables.size(), 1U);
    const auto &relocations = parsed.relocation_tables.front().relocations;
    ASSERT_EQ(relocations.size(), 8U);
    EXPECT_EQ(relocations[6], (PrxRelocation{0x10, 2, 0, 1}));
    EXPECT_EQ(relocations[7], (PrxRelocation{0, 2, 1, 0}));
}

TEST(PrxReaderTest, RejectsUnsupportedRelocationEncodingsBeforePreparation)
{
    for (const std::uint32_t info : {8U, 0x00000102U, 0x00010002U, 0x01000002U})
    {
        SCOPED_TRACE(info);
        PrxFixture fixture;
        fixture.relocation(0x10, info);
        EXPECT_THROW(read_prx(fixture.bytes), std::invalid_argument);
    }
    PrxFixture truncated;
    truncated.word(0x664, truncated.relocation_count * 8 + 1);
    EXPECT_THROW(read_prx(truncated.bytes), std::invalid_argument);
    PrxFixture compressed;
    compressed.word(0x654, 0x700000A1);
    EXPECT_THROW(read_prx(compressed.bytes), std::invalid_argument);
}

TEST(PrxReaderTest, RejectsInvalidElfHeadersAndTruncatedTables)
{
    const HeaderPatch patches[] = {
        {0, 0x5053507E},                                                        // encrypted PSP container
        {4, 0x00010102},                                                        // ELF64
        {4, 0x00010201},                                                        // big endian
        {16, 0x00080002},                                                       // ELF executable rather than PRX
        {16, 0x0003FFA0},                                                       // wrong machine
        {20, 2},          {28, 0xFFFFFFF0}, {32, 0xFFFFFFF0}, {40, 0x00200000}, // invalid ELF header size
        {44, 0x00280000},                                                       // no program headers
    };
    for (const auto &patch : patches)
    {
        SCOPED_TRACE(patch.offset);
        SCOPED_TRACE(patch.value);
        PrxFixture fixture;
        fixture.word(patch.offset, patch.value);
        EXPECT_THROW(read_prx(fixture.bytes), std::invalid_argument);
    }
    PrxFixture fixture;
    fixture.bytes.resize(51);
    EXPECT_THROW(read_prx(fixture.bytes), std::invalid_argument);
}

} // namespace psp
