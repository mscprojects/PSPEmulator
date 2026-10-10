#pragma once

#include "common/payload.hpp"
#include "memory/address.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace psp
{

// Mapped regions own independent byte storage. Aliases share storage within this
// Memory; copying Memory copies all backing bytes and preserves its alias layout.
class Memory
{
    struct Location
    {
        std::size_t region;
        std::size_t offset;
    };

public:
    Memory(GuestAddress base_address, std::size_t size);
    // Add a zeroed region, rejecting empty, overlapping, or wrapping mappings.
    void map_region(GuestAddress base_address, std::size_t size);
    // Map the entire region beginning at region_base through another address.
    // region_base must be an existing mapping start; an alias can be its source.
    // The new range must fit the address space and not overlap any mapping.
    void map_alias(GuestAddress alias_base, GuestAddress region_base);

    // Validate a complete range without copying its bytes. Empty ranges are valid.
    void validate_range(GuestAddress address, std::size_t size) const;

    std::uint8_t read_u8(GuestAddress address) const;
    std::uint16_t read_u16(GuestAddress address) const;
    std::uint32_t read_u32(GuestAddress address) const;

    // Return an owning copy after checking the entire range. An empty range is valid
    // without dereferencing its address; invalid nonempty ranges throw std::out_of_range.
    Payload read_bytes(GuestAddress address, std::size_t size) const;
    // Copy into caller-owned storage after validating the entire guest range.
    // Empty reads are no-ops; invalid ranges leave the destination unchanged.
    void read_into(GuestAddress address, std::span<std::byte> destination) const;
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
    void validate_mapping(GuestAddress base_address, std::size_t size) const;
    Location checked_location(GuestAddress address, std::size_t width) const;
    std::uint32_t read_le(GuestAddress address, std::size_t width) const;
    void write_le(GuestAddress address, std::uint32_t value, std::size_t width);

    std::vector<Payload> regions_;
    std::map<std::uint32_t, std::size_t> mappings_;
};

// Create the PSP execution layout: zeroed RAM at ram_base with shared views at address
// bits 30 and 31, plus 2 MiB of VRAM at 0x04000000 with its 0x44000000 view.
// RAM must be nonempty, fit below 0x40000000, and not overlap VRAM; otherwise this
// throws std::invalid_argument.
Memory create_psp_memory(GuestAddress ram_base, std::size_t ram_size);

} // namespace psp
