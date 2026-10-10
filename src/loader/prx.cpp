#include "loader/prx.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace psp
{

namespace
{

struct LoadedSegment
{
    const PrxSegment &header;
    GuestAddress address;
};

struct PendingHighRelocation
{
    GuestAddress address;
    std::uint32_t instruction;
    std::size_t base_segment;
};

constexpr std::size_t kModuleInfoSize = 52;
constexpr std::size_t kModuleNameSize = 28;
constexpr std::uint32_t kLoadSegment = 1;
constexpr std::uint32_t kExecutableSegment = 1;
constexpr std::uint32_t kWordSize = 4;
constexpr std::uint32_t kFunctionStubSize = 8;
constexpr std::uint32_t kImportHeaderSize = 20;
constexpr std::uint32_t kLowHalfwordMask = 0xFFFF;
constexpr std::uint32_t kInstructionUpperMask = 0xFFFF0000;
constexpr std::uint32_t kJumpTargetMask = 0x03FFFFFF;
constexpr std::uint32_t kJumpOpcodeMask = 0xFC000000;
constexpr std::uint32_t kSignedLowCarry = 0x8000;

// Reject address overflow before narrowing; loader address arithmetic must not wrap.
GuestAddress checked_address(std::uint64_t value)
{
    if (value > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument("PRX address exceeds the 32-bit address space");
    }
    return GuestAddress{static_cast<std::uint32_t>(value)};
}

// Check the full range against a load segment, including its zero-initialized BSS.
// A zero-length range may sit exactly at the segment's end.
bool contains(const LoadedSegment &segment, GuestAddress address, std::uint64_t size)
{
    if (segment.header.type != kLoadSegment || address.value_of() < segment.address.value_of())
    {
        return false;
    }
    const auto offset = static_cast<std::uint64_t>(address.value_of()) - segment.address.value_of();
    return offset <= segment.header.memory_size && size <= segment.header.memory_size - offset;
}

// Require metadata to fit in one load segment; spare RAM and gaps between segments
// are readable Memory, but are not valid storage for PRX metadata.
void require_mapped(std::span<const LoadedSegment> segments, GuestAddress address, std::uint64_t size)
{
    if (!std::ranges::any_of(segments, [&](const LoadedSegment &segment) { return contains(segment, address, size); }))
    {
        throw std::invalid_argument("PRX pointer is outside its loaded segments");
    }
}

// Reject unaligned guest instructions and tables even though Memory allows unaligned reads.
void require_word_aligned(GuestAddress address)
{
    if (address.value_of() % kWordSize != 0)
    {
        throw std::invalid_argument("PRX instruction or table address must be word aligned");
    }
}

// Choose guest addresses, validate placement, and copy owned segment bytes into zeroed memory.
// Retain non-load headers so relocation indices still refer to the original header order.
std::vector<LoadedSegment> load_segments(Memory &memory, std::span<const PrxSegment> segments,
                                         GuestAddress load_address, std::size_t memory_size)
{
    std::vector<LoadedSegment> loaded_segments;
    for (const auto &segment : segments)
    {
        LoadedSegment loaded{segment, GuestAddress{0}};
        if (segment.type == kLoadSegment)
        {
            if (segment.virtual_address > memory_size || segment.memory_size > memory_size - segment.virtual_address)
            {
                throw std::invalid_argument("PRX segment does not fit in guest memory");
            }
            // PRX virtual addresses are offsets from the chosen guest load address.
            loaded.address =
                checked_address(static_cast<std::uint64_t>(load_address.value_of()) + segment.virtual_address);
            if (segment.alignment > 1 &&
                (!std::has_single_bit(segment.alignment) || load_address.value_of() % segment.alignment != 0))
            {
                throw std::invalid_argument("Invalid PRX load address alignment");
            }
            for (const auto &other : loaded_segments)
            {
                if (other.header.type == kLoadSegment && segment.memory_size != 0 && other.header.memory_size != 0 &&
                    static_cast<std::uint64_t>(loaded.address.value_of()) + segment.memory_size >
                        other.address.value_of() &&
                    static_cast<std::uint64_t>(other.address.value_of()) + other.header.memory_size >
                        loaded.address.value_of())
                {
                    throw std::invalid_argument("Overlapping PRX load segments are unsupported");
                }
            }
            if (segment.bytes.size() > segment.memory_size)
            {
                throw std::invalid_argument("PRX segment contents exceed its memory size");
            }
            // The rest of p_memsz is BSS; zeroed Memory also preserves gaps and spare RAM.
            memory.write_bytes(loaded.address, segment.bytes);
        }
        loaded_segments.push_back(loaded);
    }
    return loaded_segments;
}

// Patch guest memory using each record's target segment and relocation base.
// Defer HI16 patches until a matching LO16 supplies the signed low half.
void relocate(Memory &memory, std::span<const LoadedSegment> segments, const PrxRelocationTable &table)
{
    std::vector<PendingHighRelocation> pending;
    for (const auto &relocation : table.relocations)
    {
        const auto type = relocation.type;
        const auto patch_segment = relocation.patch_segment;
        const auto base_segment = relocation.base_segment;
        if (patch_segment >= segments.size() || base_segment >= segments.size() ||
            segments[patch_segment].header.type != kLoadSegment || segments[base_segment].header.type != kLoadSegment)
        {
            throw std::invalid_argument("Invalid PSP relocation segment index");
        }
        const auto address =
            checked_address(static_cast<std::uint64_t>(segments[patch_segment].address.value_of()) + relocation.offset);
        if (!contains(segments[patch_segment], address, kWordSize))
        {
            throw std::invalid_argument("PSP relocation exceeds its target segment");
        }
        if (type != 2) // R_MIPS_32 permits an unaligned data word.
        {
            require_word_aligned(address);
        }
        const auto instruction = memory.read_u32(address);
        const auto base = segments[base_segment].address.value_of();
        switch (type)
        {
        case 1: // R_MIPS_16
            memory.write_u32(address,
                             (instruction & kInstructionUpperMask) | ((instruction + base) & kLowHalfwordMask));
            break;
        case 2: // R_MIPS_32
            memory.write_u32(address, instruction + base);
            break;
        case 4: // R_MIPS_26: preserve the J/JAL opcode while relocating the word target.
            require_word_aligned(segments[base_segment].address);
            // The encoded target counts words; the segment base is a byte address.
            memory.write_u32(address, (instruction & kJumpOpcodeMask) |
                                          (((instruction & kJumpTargetMask) + base / kWordSize) & kJumpTargetMask));
            break;
        case 5: // R_MIPS_HI16: several LUI instructions can share one following LO16.
            pending.push_back({address, instruction, base_segment});
            break;
        case 6: // R_MIPS_LO16
        {
            const auto low =
                static_cast<std::int32_t>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(instruction)));
            for (const auto &high : pending)
            {
                if (high.base_segment == base_segment)
                {
                    const auto value =
                        ((high.instruction & kLowHalfwordMask) << 16) + static_cast<std::uint32_t>(low) + base;
                    // LUI must compensate when the paired instruction sign-extends its low half.
                    const auto relocated_high = ((value + kSignedLowCarry) >> 16) & kLowHalfwordMask;
                    memory.write_u32(high.address, (high.instruction & kInstructionUpperMask) | relocated_high);
                }
            }
            std::erase_if(pending, [&](const auto &high) { return high.base_segment == base_segment; });
            memory.write_u32(address,
                             (instruction & kInstructionUpperMask) | ((instruction + base) & kLowHalfwordMask));
            break;
        }
        case 7: // R_MIPS_GPREL16: already GP-relative; PSP leaves these words unchanged.
            break;
        default:
            throw std::invalid_argument("Unsupported PSP relocation type");
        }
    }
    if (!pending.empty())
    {
        throw std::invalid_argument("PSP HI16 relocation has no matching LO16");
    }
}

// Read a terminated import-library name, validating each byte against loaded storage.
std::string read_name(const Memory &memory, std::span<const LoadedSegment> segments, GuestAddress address)
{
    std::string name;
    // Import names have no declared length. Require a terminator before walking
    // outside loaded storage, even though the surrounding guest RAM is zeroed.
    for (auto cursor = static_cast<std::uint64_t>(address.value_of());; ++cursor)
    {
        const auto current = checked_address(cursor);
        require_mapped(segments, current, 1);
        const auto character = memory.read_u8(current);
        if (character == 0)
        {
            return name;
        }
        name.push_back(static_cast<char>(character));
    }
}

// Read relocated module fields at the parsed offset within the first load segment.
PrxModule read_module(const Memory &memory, std::span<const LoadedSegment> segments, std::uint32_t module_offset)
{
    const auto &segment = segments.front();
    if (module_offset > segment.header.bytes.size() || kModuleInfoSize > segment.header.bytes.size() - module_offset)
    {
        throw std::invalid_argument("PRX module information is outside the first segment");
    }
    // The reader already converted the module location to a segment-relative offset.
    // Pointer fields below are interpreted only after their relocations have been applied.
    const auto address = checked_address(static_cast<std::uint64_t>(segment.address.value_of()) + module_offset);
    require_word_aligned(address);
    std::string name;
    for (std::size_t index = 0; index < kModuleNameSize; ++index)
    {
        const auto character =
            memory.read_u8(GuestAddress{address.value_of() + kWordSize + static_cast<std::uint32_t>(index)});
        if (character == 0)
        {
            break;
        }
        name.push_back(static_cast<char>(character));
    }
    if (name.size() == kModuleNameSize)
    {
        throw std::invalid_argument("PRX module name is not terminated");
    }
    // Module-info layout: attributes at 0, version at 2, name at 4 (28 bytes),
    // GP at 32, export range at 36/40, and import range at 44/48.
    return {std::move(name),
            memory.read_u16(address),
            memory.read_u16(GuestAddress{address.value_of() + 2}),
            GuestAddress{memory.read_u32(GuestAddress{address.value_of() + 32})},
            GuestAddress{memory.read_u32(GuestAddress{address.value_of() + 36})},
            GuestAddress{memory.read_u32(GuestAddress{address.value_of() + 40})}};
}

// Describe unresolved function imports from relocated library records. Validate their
// names and tables without changing stubs; variable imports are unsupported.
std::vector<PrxImportLibrary> read_imports(const Memory &memory, std::span<const LoadedSegment> segments,
                                           std::uint32_t module_offset)
{
    const auto &segment = segments.front();
    const auto module_address = segment.address.value_of() + module_offset;
    auto cursor = memory.read_u32(GuestAddress{module_address + 44});
    const auto end = memory.read_u32(GuestAddress{module_address + 48});
    if (end < cursor)
    {
        throw std::invalid_argument("PRX import range is reversed");
    }
    if (cursor != 0 || end != 0)
    {
        require_word_aligned(GuestAddress{cursor});
        require_word_aligned(GuestAddress{end});
        require_mapped(segments, GuestAddress{cursor}, end - cursor);
    }
    std::vector<PrxImportLibrary> imports;
    while (cursor < end)
    {
        if (end - cursor < kImportHeaderSize)
        {
            throw std::invalid_argument("Truncated PRX import library header");
        }
        // The record length is encoded in words. Accept the 5- and 6-word layouts,
        // advancing by that length rather than assuming every record is 20 bytes.
        const auto size = memory.read_u8(GuestAddress{cursor + 8}) * kWordSize;
        const auto variable_count = memory.read_u8(GuestAddress{cursor + 9});
        if ((size != kImportHeaderSize && size != kImportHeaderSize + kWordSize) || size > end - cursor ||
            variable_count != 0)
        {
            throw std::invalid_argument("Unsupported PRX import library layout or variable imports");
        }
        const auto name_address = GuestAddress{memory.read_u32(GuestAddress{cursor})};
        require_mapped(segments, name_address, 1);
        PrxImportLibrary library{read_name(memory, segments, name_address),
                                 memory.read_u16(GuestAddress{cursor + 4}),
                                 memory.read_u16(GuestAddress{cursor + 6}),
                                 {}};
        // The NID and stub tables are parallel: each identifier describes the
        // function requested by the corresponding guest instruction stub.
        const auto count = memory.read_u16(GuestAddress{cursor + 10});
        const auto nids = memory.read_u32(GuestAddress{cursor + 12});
        const auto stubs = memory.read_u32(GuestAddress{cursor + 16});
        if (count != 0)
        {
            require_word_aligned(GuestAddress{nids});
            require_word_aligned(GuestAddress{stubs});
            require_mapped(segments, GuestAddress{nids}, static_cast<std::uint64_t>(count) * kWordSize);
            require_mapped(segments, GuestAddress{stubs}, static_cast<std::uint64_t>(count) * kFunctionStubSize);
        }
        for (std::uint32_t index = 0; index < count; ++index)
        {
            library.functions.push_back({memory.read_u32(GuestAddress{nids + index * kWordSize}),
                                         GuestAddress{stubs + index * kFunctionStubSize}});
        }
        imports.push_back(std::move(library));
        cursor += size;
    }
    return imports;
}

} // namespace

// Place parsed segments, apply relocations, then decode guest metadata.
// Guest memory stays local until every stage succeeds, so failures expose no partial image.
LoadedPrx prepare_prx(const ParsedPrx &prx, GuestAddress load_address, std::size_t memory_size)
{
    if (prx.segments.empty() || prx.segments.front().type != kLoadSegment)
    {
        throw std::invalid_argument("PRX must begin with a load segment containing module information");
    }
    require_word_aligned(load_address);
    auto memory = create_psp_memory(load_address, memory_size);
    const auto segments = load_segments(memory, prx.segments, load_address, memory_size);
    // load_segments() checked that every load segment fits in RAM, so its end cannot wrap.
    auto image_end = load_address.value_of();
    for (const auto &segment : segments)
    {
        if (segment.header.type == kLoadSegment)
        {
            image_end = std::max(image_end, segment.address.value_of() + segment.header.memory_size);
        }
    }
    const auto entry = checked_address(static_cast<std::uint64_t>(load_address.value_of()) + prx.entry_offset);
    require_word_aligned(entry);
    if (!std::ranges::any_of(
            segments, [&](const auto &segment)
            { return contains(segment, entry, kWordSize) && (segment.header.flags & kExecutableSegment) != 0; }))
    {
        throw std::invalid_argument("PRX entry point is outside executable segments");
    }
    // Module and import records contain pointers that must be relocated before
    // they can be interpreted as guest addresses.
    for (const auto &table : prx.relocation_tables)
    {
        relocate(memory, segments, table);
    }
    auto module = read_module(memory, segments, prx.module_offset);
    // PSPSDK's linker biases _gp by 0x7ff0 for signed small-data offsets.
    // GP is an address base, so GP itself need not lie inside a loaded segment.
    if (module.exports_end.value_of() < module.exports_begin.value_of())
    {
        throw std::invalid_argument("PRX export range is reversed");
    }
    if (module.exports_begin.value_of() != 0 || module.exports_end.value_of() != 0)
    {
        require_word_aligned(module.exports_begin);
        require_word_aligned(module.exports_end);
        require_mapped(segments, module.exports_begin, module.exports_end.value_of() - module.exports_begin.value_of());
    }
    auto imports = read_imports(memory, segments, prx.module_offset);
    return {std::move(memory), entry, GuestAddress{image_end}, std::move(module), std::move(imports)};
}

} // namespace psp
