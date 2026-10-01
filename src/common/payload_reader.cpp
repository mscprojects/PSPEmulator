#include "common/payload_reader.hpp"

#include <stdexcept>

namespace psp
{

PayloadReader::PayloadReader(PayloadSpan payload, std::endian endianness) : payload_(payload), endianness_(endianness)
{
    if (endianness != std::endian::little && endianness != std::endian::big)
    {
        throw std::invalid_argument("Payload byte order must be little or big endian");
    }
}

std::size_t PayloadReader::position() const
{
    return position_;
}

std::size_t PayloadReader::remaining() const
{
    return payload_.size() - position_;
}

void PayloadReader::seek(std::size_t offset)
{
    require_range(offset, 0);
    position_ = offset;
}

void PayloadReader::skip(std::size_t size)
{
    require_range(position_, size);
    position_ += size;
}

void PayloadReader::require_range(std::size_t offset, std::size_t size) const
{
    // Subtraction avoids overflowing offset + size for malformed ranges.
    if (offset > payload_.size() || size > payload_.size() - offset)
    {
        throw std::invalid_argument("Payload range is truncated");
    }
}

PayloadSpan PayloadReader::read_bytes(std::size_t size)
{
    require_range(position_, size);
    const auto bytes = payload_.subspan(position_, size);
    position_ += size;
    return bytes;
}

std::uint8_t PayloadReader::read_u8()
{
    return read_bytes(1)[0];
}

std::uint16_t PayloadReader::read_u16()
{
    const auto bytes = read_bytes(2);
    if (endianness_ == std::endian::little)
    {
        return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t PayloadReader::read_u32()
{
    const auto bytes = read_bytes(4);
    if (endianness_ == std::endian::little)
    {
        return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

} // namespace psp
