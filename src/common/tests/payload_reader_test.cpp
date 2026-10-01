#include "common/payload_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace psp
{

TEST(PayloadReaderTest, ReadsSequentiallyInEitherByteOrder)
{
    const std::array<std::uint8_t, 7> bytes{0xAB, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    PayloadReader little(bytes, std::endian::little);
    EXPECT_EQ(little.read_u8(), 0xAB);
    EXPECT_EQ(little.read_u16(), 0x3412);
    EXPECT_EQ(little.read_u32(), 0xBC9A7856U);
    EXPECT_EQ(little.position(), bytes.size());
    EXPECT_EQ(little.remaining(), 0U);

    PayloadReader big(bytes, std::endian::big);
    EXPECT_EQ(big.read_u8(), 0xAB);
    EXPECT_EQ(big.read_u16(), 0x1234);
    EXPECT_EQ(big.read_u32(), 0x56789ABCU);
    EXPECT_EQ(big.position(), bytes.size());
}

TEST(PayloadReaderTest, ReturnsBorrowedBytesAndSupportsBoundedReaders)
{
    Payload bytes{0x12, 0x34, 0x56, 0x78};
    PayloadReader parent(bytes, std::endian::little);
    const PayloadSpan record = parent.read_bytes(2);
    EXPECT_EQ(record.data(), bytes.data());
    EXPECT_EQ(parent.position(), 2U);
    bytes[0] = 0xAB;

    PayloadReader child(record, std::endian::big);
    EXPECT_EQ(child.read_u16(), 0xAB34);
    EXPECT_THROW(static_cast<void>(child.read_u8()), std::invalid_argument);
    EXPECT_EQ(parent.read_u16(), 0x7856);
}

TEST(PayloadReaderTest, FailedReadsAndSeeksPreserveThePosition)
{
    const std::array<std::uint8_t, 3> bytes{0x12, 0x34, 0x56};
    PayloadReader reader(bytes, std::endian::little);
    reader.seek(2);
    EXPECT_THROW(static_cast<void>(reader.read_u16()), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(reader.read_u32()), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(reader.read_bytes(2)), std::invalid_argument);
    EXPECT_THROW(reader.seek(4), std::invalid_argument);
    EXPECT_THROW(reader.skip(2), std::invalid_argument);
    EXPECT_THROW(reader.require_range(1, std::numeric_limits<std::size_t>::max()), std::invalid_argument);
    EXPECT_THROW(reader.seek(std::numeric_limits<std::size_t>::max()), std::invalid_argument);
    EXPECT_THROW(reader.skip(std::numeric_limits<std::size_t>::max()), std::invalid_argument);
    EXPECT_EQ(reader.position(), 2U);
    EXPECT_EQ(reader.read_u8(), 0x56);

    reader.seek(0);
    EXPECT_EQ(reader.read_u16(), 0x3412);
}

TEST(PayloadReaderTest, SupportsEmptyPayloadsAndEndPositions)
{
    PayloadReader empty({}, std::endian::little);
    empty.seek(0);
    empty.skip(0);
    EXPECT_TRUE(empty.read_bytes(0).empty());
    EXPECT_EQ(empty.remaining(), 0U);
    EXPECT_THROW(static_cast<void>(empty.read_u8()), std::invalid_argument);
    EXPECT_EQ(empty.position(), 0U);

    const std::array<std::uint8_t, 1> bytes{0xAB};
    PayloadReader reader(bytes, std::endian::little);
    reader.seek(bytes.size());
    reader.skip(0);
    EXPECT_TRUE(reader.read_bytes(0).empty());
    EXPECT_THROW(static_cast<void>(reader.read_u8()), std::invalid_argument);
    EXPECT_EQ(reader.position(), bytes.size());
}

TEST(PayloadReaderTest, RejectsInvalidByteOrder)
{
    const std::array<std::uint8_t, 4> bytes{0x12, 0x34, 0x56, 0x78};
    // Exercise rejection of an unsupported enum value deliberately.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_THROW((PayloadReader{bytes, static_cast<std::endian>(0)}), std::invalid_argument);
}

TEST(PayloadReaderTest, SkipsRelativeToTheCurrentPosition)
{
    const std::array<std::uint8_t, 6> bytes{0xAB, 0xFF, 0xFF, 0x12, 0x34, 0x56};
    PayloadReader reader(bytes, std::endian::little);
    EXPECT_EQ(reader.read_u8(), 0xAB);
    reader.skip(2);
    EXPECT_EQ(reader.position(), 3U);
    EXPECT_EQ(reader.read_u16(), 0x3412);
    EXPECT_EQ(reader.remaining(), 1U);
    reader.skip(1);
    EXPECT_EQ(reader.remaining(), 0U);
}

} // namespace psp
