#include "core/memory.hpp"

#include <limits>
#include <stdexcept>

namespace psp
{

Memory::Memory(std::uint32_t base_address, std::size_t size) : base_address_(base_address)
{
    constexpr auto address_space_size = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    if (size == 0 || static_cast<std::uint64_t>(size) > address_space_size - base_address)
    {
        throw std::invalid_argument("Memory range must fit in the 32-bit address space");
    }
    bytes_.resize(size);
}

std::uint8_t Memory::read_u8(std::uint32_t address) const
{
    return static_cast<std::uint8_t>(read_le(address, 1));
}

std::uint16_t Memory::read_u16(std::uint32_t address) const
{
    return static_cast<std::uint16_t>(read_le(address, 2));
}

std::uint32_t Memory::read_u32(std::uint32_t address) const
{
    return read_le(address, 4);
}

void Memory::write_u8(std::uint32_t address, std::uint8_t value)
{
    write_le(address, value, 1);
}

void Memory::write_u16(std::uint32_t address, std::uint16_t value)
{
    write_le(address, value, 2);
}

void Memory::write_u32(std::uint32_t address, std::uint32_t value)
{
    write_le(address, value, 4);
}

std::size_t Memory::checked_offset(std::uint32_t address, std::size_t width) const
{
    if (address < base_address_)
    {
        throw std::out_of_range("Memory address is below the mapped region");
    }
    const auto offset = static_cast<std::size_t>(address - base_address_);
    if (offset >= bytes_.size() || width > bytes_.size() - offset)
    {
        throw std::out_of_range("Memory access exceeds the mapped region");
    }
    return offset;
}

std::uint32_t Memory::read_le(std::uint32_t address, std::size_t width) const
{
    const auto offset = checked_offset(address, width);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
    {
        value |= static_cast<std::uint32_t>(bytes_[offset + index]) << (index * 8);
    }
    return value;
}

void Memory::write_le(std::uint32_t address, std::uint32_t value, std::size_t width)
{
    const auto offset = checked_offset(address, width);
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

} // namespace psp
