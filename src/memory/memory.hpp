#pragma once

#include "memory/address.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp
{

class Memory
{
public:
    Memory(GuestAddress base_address, std::size_t size);

    std::uint8_t read_u8(GuestAddress address) const;
    std::uint16_t read_u16(GuestAddress address) const;
    std::uint32_t read_u32(GuestAddress address) const;

    void write_u8(GuestAddress address, std::uint8_t value);
    void write_u16(GuestAddress address, std::uint16_t value);
    void write_u32(GuestAddress address, std::uint32_t value);

private:
    enum class AccessWidth : std::uint8_t
    {
        Byte = 1,
        Halfword = 2,
        Word = 4,
    };

    std::size_t checked_offset(GuestAddress address, std::size_t width) const;
    std::uint32_t read_le(GuestAddress address, AccessWidth width) const;
    void write_le(GuestAddress address, std::uint32_t value, AccessWidth width);

    GuestAddress base_address_;
    std::vector<std::uint8_t> bytes_;
};

} // namespace psp
