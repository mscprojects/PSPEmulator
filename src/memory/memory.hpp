#pragma once

#include "common/payload.hpp"
#include "memory/address.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace psp
{

class Memory
{
public:
    Memory(GuestAddress base_address, std::size_t size);

    std::uint8_t read_u8(GuestAddress address) const;
    std::uint16_t read_u16(GuestAddress address) const;
    std::uint32_t read_u32(GuestAddress address) const;

    // Return an owning copy after checking the entire range. An empty range is valid
    // without dereferencing its address; invalid nonempty ranges throw std::out_of_range.
    Payload read_bytes(GuestAddress address, std::size_t size) const;
    // Copy a NUL-terminated string, excluding its terminator from the result.
    // max_length bounds the bytes examined, including the terminator.
    // Throws std::invalid_argument for a zero limit, std::out_of_range if reading
    // would leave memory, or std::runtime_error if the limit is reached without a NUL.
    std::string read_c_string(GuestAddress address, std::size_t max_length) const;

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
