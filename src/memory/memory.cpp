#include "memory/memory.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace psp
{

Memory::Memory(GuestAddress base_address, std::size_t size)
{
    map_region(base_address, size);
}

void Memory::map_region(GuestAddress base_address, std::size_t size)
{
    validate_mapping(base_address, size);
    regions_.emplace_back(size);
    try
    {
        mappings_.emplace(base_address.value_of(), regions_.size() - 1);
    }
    catch (...)
    {
        regions_.pop_back();
        throw;
    }
}

// These guest addresses identify the new view and its existing backing region.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Memory::map_alias(GuestAddress alias_base, GuestAddress region_base)
{
    const auto region = mappings_.find(region_base.value_of());
    if (region == mappings_.end())
    {
        throw std::invalid_argument("Alias source must be an existing mapping start");
    }
    validate_mapping(alias_base, regions_[region->second].size());
    mappings_.emplace(alias_base.value_of(), region->second);
}

void Memory::validate_range(GuestAddress address, std::size_t size) const
{
    if (size != 0)
    {
        checked_location(address, size);
    }
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
    const auto location = checked_location(address, size);
    const auto bytes = PayloadSpan{regions_[location.region]}.subspan(location.offset, size);
    return Payload(bytes.begin(), bytes.end());
}

void Memory::read_into(GuestAddress address, std::span<std::byte> destination) const
{
    if (destination.empty())
    {
        return;
    }
    const auto location = checked_location(address, destination.size());
    std::memcpy(destination.data(), regions_[location.region].data() + location.offset, destination.size());
}

std::string Memory::read_c_string(GuestAddress address, std::size_t max_length) const
{
    if (max_length == 0)
    {
        throw std::invalid_argument("String read limit must be positive");
    }
    const auto location = checked_location(address, 1);
    const auto remaining = PayloadSpan{regions_[location.region]}.subspan(location.offset);
    const auto bytes = remaining.first(std::min(max_length, remaining.size()));
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
    const auto location = checked_location(address, bytes.size());
    std::ranges::copy(bytes, regions_[location.region].begin() + static_cast<std::ptrdiff_t>(location.offset));
}

void Memory::validate_mapping(GuestAddress base_address, std::size_t size) const
{
    constexpr auto address_space_size = std::uint64_t{1} << 32;
    const auto base = static_cast<std::uint64_t>(base_address.value_of());
    if (size == 0 || size > address_space_size - base)
    {
        throw std::invalid_argument("Memory range must fit in the 32-bit address space");
    }
    const auto next = mappings_.lower_bound(base_address.value_of());
    if (next != mappings_.end() && base + size > next->first)
    {
        throw std::invalid_argument("Memory mapping overlaps an existing region");
    }
    if (next != mappings_.begin())
    {
        const auto previous = std::prev(next);
        if (base < static_cast<std::uint64_t>(previous->first) + regions_[previous->second].size())
        {
            throw std::invalid_argument("Memory mapping overlaps an existing region");
        }
    }
}

Memory::Location Memory::checked_location(GuestAddress address, std::size_t width) const
{
    auto mapping = mappings_.upper_bound(address.value_of());
    if (mapping == mappings_.begin())
    {
        throw std::out_of_range("Memory address is outside mapped regions");
    }
    --mapping;
    const auto offset = static_cast<std::size_t>(address.value_of() - mapping->first);
    const auto size = regions_[mapping->second].size();
    if (offset >= size || width > size - offset)
    {
        throw std::out_of_range("Memory access exceeds the mapped region");
    }
    return {mapping->second, offset};
}

std::uint32_t Memory::read_le(GuestAddress address, std::size_t width) const
{
    const auto location = checked_location(address, width);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
    {
        value |= static_cast<std::uint32_t>(regions_[location.region][location.offset + index]) << (index * 8);
    }
    return value;
}

// The width is fixed at each private call site, and this order matches the public write methods.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Memory::write_le(GuestAddress address, std::uint32_t value, std::size_t width)
{
    const auto location = checked_location(address, width);
    for (std::size_t index = 0; index < width; ++index)
    {
        regions_[location.region][location.offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

} // namespace psp
