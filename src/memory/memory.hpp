#pragma once

#include "common/payload.hpp"
#include "memory/address.hpp"

#include <cstddef>
#include <cstdint>

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
    // Copy a guest byte range after checking its full extent. An empty range is a no-op.
    void write_bytes(GuestAddress address, PayloadSpan bytes);

private:
    std::size_t checked_offset(GuestAddress address, std::size_t width) const;
    std::uint32_t read_le(GuestAddress address, std::size_t width) const;
    void write_le(GuestAddress address, std::uint32_t value, std::size_t width);

    GuestAddress base_address_;
    Payload bytes_;
};

} // namespace psp
