#include "memory/memory.hpp"

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
    return static_cast<std::uint8_t>(read_le(address, AccessWidth::Byte));
}

std::uint16_t Memory::read_u16(GuestAddress address) const
{
    return static_cast<std::uint16_t>(read_le(address, AccessWidth::Halfword));
}

std::uint32_t Memory::read_u32(GuestAddress address) const
{
    return read_le(address, AccessWidth::Word);
}

void Memory::write_u8(GuestAddress address, std::uint8_t value)
{
    write_le(address, value, AccessWidth::Byte);
}

void Memory::write_u16(GuestAddress address, std::uint16_t value)
{
    write_le(address, value, AccessWidth::Halfword);
}

void Memory::write_u32(GuestAddress address, std::uint32_t value)
{
    write_le(address, value, AccessWidth::Word);
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

std::uint32_t Memory::read_le(GuestAddress address, AccessWidth access_width) const
{
    const auto width = static_cast<std::size_t>(access_width);
    const auto offset = checked_offset(address, width);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
    {
        value |= static_cast<std::uint32_t>(bytes_[offset + index]) << (index * 8);
    }
    return value;
}

void Memory::write_le(GuestAddress address, std::uint32_t value, AccessWidth access_width)
{
    const auto width = static_cast<std::size_t>(access_width);
    const auto offset = checked_offset(address, width);
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

} // namespace psp
