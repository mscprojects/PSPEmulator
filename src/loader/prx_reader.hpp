#pragma once

#include "common/payload.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp
{

// ELF PT_LOAD: a segment whose contents are placed in guest memory.
inline constexpr std::uint32_t kPrxLoadSegment = 1;
// Size of the PSP module-info record, located inside the first load segment.
inline constexpr std::uint32_t kPrxModuleInfoSize = 52;

// Uncompressed PSP relocation types, numbered as the MIPS ELF R_MIPS_* types.
enum class PrxRelocationType : std::uint32_t
{
    Mips16 = 1,
    Mips32 = 2,
    Mips26 = 4,
    MipsHigh16 = 5,
    MipsLow16 = 6,
    MipsGpRelative16 = 7,
};

// A segment ready for guest placement. virtual_address is relative to the load base;
// bytes owns the initialized contents, and memory_size also includes zero-filled BSS.
struct PrxSegment
{
    std::uint32_t type;
    std::uint32_t virtual_address;
    std::uint32_t memory_size;
    std::uint32_t flags;
    std::uint32_t alignment;
    Payload bytes;
    bool operator==(const PrxSegment &) const = default;
};

struct PrxRelocation
{
    // Byte offset within patch_segment, not within the file or final guest memory.
    std::uint32_t offset;
    PrxRelocationType type;
    std::size_t patch_segment;
    std::size_t base_segment;
    bool operator==(const PrxRelocation &) const = default;
};

struct PrxRelocationTable
{
    // Preserve record order and table boundaries for HI16/LO16 pairing.
    std::vector<PrxRelocation> relocations;
};

struct ParsedPrx
{
    std::uint32_t entry_offset;
    // Byte offset of module information within the first load segment.
    std::uint32_t module_offset;
    // Original program-header order, including non-load headers used by relocation indices.
    std::vector<PrxSegment> segments;
    std::vector<PrxRelocationTable> relocation_tables;
};

// Read a plain little-endian ELF32 PSP PRX without allocating guest memory or
// choosing load addresses. The result owns all data needed for preparation, so the
// input can be released after this call. Throws std::invalid_argument for malformed file structure
// or unsupported formats. Placement and relocated metadata are checked by prepare_prx().
ParsedPrx read_prx(PayloadSpan payload);

} // namespace psp
