#include "memory/memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace psp
{

Memory::Memory(GuestAddress base_address, std::size_t size) : base_address_(base_address)
{
    constexpr auto address_space_size = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    if (size == 0 || static_cast<std::uint64_t>(size) > address_space_size - base_address.value_of())
    {
        throw std::invalid_argument("Memory range must fit in the 32-bit address space");
    }
    bytes_.resize(size);
}

std::uint8_t Memory::read_u8(GuestAddress address) const
{
    return static_cast<std::uint8_t>(read_le(address, 1));
}

std::uint16_t Memory::read_u16(GuestAddress address) const
{
    return static_cast<std::uint16_t>(read_le(address, 2));
}

std::uint32_t Memory::read_u32(GuestAddress address) const
{
    return read_le(address, 4);
}

Payload Memory::read_bytes(GuestAddress address, std::size_t size) const
{
    if (size == 0)
    {
        return {};
    }
    const auto offset = checked_offset(address, size);
    const auto bytes = PayloadSpan{bytes_}.subspan(offset, size);
    return Payload(bytes.begin(), bytes.end());
}

std::string Memory::read_c_string(GuestAddress address, std::size_t max_length) const
{
    if (max_length == 0)
    {
        throw std::invalid_argument("String read limit must be positive");
    }
    const auto offset = checked_offset(address, 1);
    const auto bytes = PayloadSpan{bytes_}.subspan(offset, std::min(max_length, bytes_.size() - offset));
    const auto terminator = std::ranges::find(bytes, 0);
    if (terminator != bytes.end())
    {
        return std::string(bytes.begin(), terminator);
    }
    if (max_length > bytes.size())
    {
        throw std::out_of_range("String read exceeds the mapped region");
    }
    throw std::runtime_error("String read limit reached without a NUL terminator");
}

void Memory::write_u8(GuestAddress address, std::uint8_t value)
{
    write_le(address, value, 1);
}

void Memory::write_u16(GuestAddress address, std::uint16_t value)
{
    write_le(address, value, 2);
}

void Memory::write_u32(GuestAddress address, std::uint32_t value)
{
    write_le(address, value, 4);
}

void Memory::write_bytes(GuestAddress address, PayloadSpan bytes)
{
    if (bytes.empty())
    {
        return;
    }
    const auto offset = checked_offset(address, bytes.size());
    std::ranges::copy(bytes, bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::size_t Memory::checked_offset(GuestAddress address, std::size_t width) const
{
    if (address.value_of() < base_address_.value_of())
    {
        throw std::out_of_range("Memory address is below the mapped region");
    }
    const auto offset = static_cast<std::size_t>(address.value_of() - base_address_.value_of());
    if (offset >= bytes_.size() || width > bytes_.size() - offset)
    {
        throw std::out_of_range("Memory access exceeds the mapped region");
    }
    return offset;
}

std::uint32_t Memory::read_le(GuestAddress address, std::size_t width) const
{
    const auto offset = checked_offset(address, width);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
    {
        value |= static_cast<std::uint32_t>(bytes_[offset + index]) << (index * 8);
    }
    return value;
}

// The width is fixed at each private call site, and this order matches the public write methods.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Memory::write_le(GuestAddress address, std::uint32_t value, std::size_t width)
{
    const auto offset = checked_offset(address, width);
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

} // namespace psp
