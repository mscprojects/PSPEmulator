#include "loader/prx_reader.hpp"

#include "common/payload_reader.hpp"

#include <bit>
#include <stdexcept>
#include <utility>

namespace psp
{

namespace
{

// Raw ELF fields stay local to file parsing; preparation never needs file offsets.
struct ProgramHeader
{
    std::uint32_t type;
    std::uint32_t file_offset;
    std::uint32_t virtual_address;
    std::uint32_t module_offset;
    std::uint32_t file_size;
    std::uint32_t memory_size;
    std::uint32_t flags;
    std::uint32_t alignment;
};

struct RelocationRange
{
    std::uint32_t offset;
    std::uint32_t size;
};

constexpr std::size_t kElfHeaderSize = 52;
constexpr std::size_t kProgramHeaderSize = 32;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kRelocationSize = 8;
constexpr std::uint32_t kModuleOffsetMask = 0x7FFFFFFF;
constexpr std::uint32_t kElfMagic = 0x464C457F;
constexpr std::uint8_t kElfClass32 = 1;
constexpr std::uint8_t kLittleEndianEncoding = 1;
constexpr std::uint32_t kElfVersion = 1;
constexpr std::uint16_t kPrxType = 0xFFA0;
constexpr std::uint16_t kMipsMachine = 8;
constexpr std::uint32_t kPspRelocations = 0x700000A0;
constexpr std::uint32_t kCompressedPspRelocations = 0x700000A1;
constexpr std::uint32_t kRelSection = 9;
constexpr std::uint32_t kRelaSection = 4;
constexpr std::uint32_t kAllocatedSection = 2;

// Parse file headers and validate their source ranges without choosing guest addresses.
// Preserve all program headers in file order for PSP relocation indices.
std::vector<ProgramHeader> parse_segments(PayloadReader file)
{
    file.seek(28);
    const auto table_offset = file.read_u32(); // e_phoff
    file.skip(10);                             // Skip section-table offset, flags, and ELF header size.
    const auto entry_size = file.read_u16();   // e_phentsize
    const auto count = file.read_u16();        // e_phnum
    if (count == 0 || count == 0xFFFF || entry_size != kProgramHeaderSize)
    {
        throw std::invalid_argument("Unsupported PRX program header table");
    }
    file.require_range(table_offset, count * kProgramHeaderSize);
    std::vector<ProgramHeader> segments;
    for (std::size_t index = 0; index < count; ++index)
    {
        file.seek(table_offset + index * kProgramHeaderSize);
        // ELF32 program header: type, file offset, virtual address, physical address,
        // file size, memory size, flags, alignment. PSP repurposes the physical address
        // field as the module-info file offset.
        ProgramHeader segment{file.read_u32(), file.read_u32(), file.read_u32(), file.read_u32(),
                              file.read_u32(), file.read_u32(), file.read_u32(), file.read_u32()};
        if (segment.type == kCompressedPspRelocations)
        {
            throw std::invalid_argument("Compressed PSP relocations are unsupported");
        }
        if (segment.type == kPrxLoadSegment)
        {
            if (segment.file_size > segment.memory_size)
            {
                throw std::invalid_argument("PRX segment file size exceeds its memory size");
            }
            file.require_range(segment.file_offset, segment.file_size);
            if (segment.alignment > 1 &&
                (!std::has_single_bit(segment.alignment) ||
                 segment.virtual_address % segment.alignment != segment.file_offset % segment.alignment))
            {
                throw std::invalid_argument("Invalid PRX segment alignment");
            }
        }
        segments.push_back(segment);
    }
    if (segments.front().type != kPrxLoadSegment)
    {
        throw std::invalid_argument("PRX must begin with a load segment containing module information");
    }
    return segments;
}

// Locate and validate uncompressed PSP relocation records from sections or segments.
// Generic ELF relocations and compressed PSP records are unsupported.
std::vector<RelocationRange> read_relocation_tables(PayloadReader file, std::span<const ProgramHeader> segments)
{
    std::vector<RelocationRange> tables;
    file.seek(32);
    const auto table_offset = file.read_u32(); // e_shoff
    file.skip(10);                             // Skip flags, ELF header size, and program-table size/count.
    const auto entry_size = file.read_u16();   // e_shentsize
    const auto count = file.read_u16();        // e_shnum
    if (count != 0)
    {
        if (entry_size != kSectionHeaderSize)
        {
            throw std::invalid_argument("Unsupported PRX section header table");
        }
        file.require_range(table_offset, count * kSectionHeaderSize);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto offset = table_offset + index * kSectionHeaderSize;
            file.seek(offset + 4);
            const auto type = file.read_u32();
            if (type == kRelSection || type == kRelaSection || type == kCompressedPspRelocations)
            {
                throw std::invalid_argument("Unsupported PRX relocation section format");
            }
            if (type != kPspRelocations)
            {
                continue;
            }
            file.seek(offset + 28);
            const auto target = file.read_u32(); // sh_info identifies the relocated section.
            file.skip(4);                        // sh_addralign
            if (target >= count || file.read_u32() != kRelocationSize)
            {
                throw std::invalid_argument("Invalid PSP relocation section header");
            }
            file.seek(table_offset + target * kSectionHeaderSize + 8);
            if ((file.read_u32() & kAllocatedSection) != 0)
            {
                file.seek(offset + 16);
                tables.push_back({file.read_u32(), file.read_u32()});
            }
        }
    }
    // Stripped images can carry relocations in program headers instead of sections.
    // Reject mixed sources so the loader cannot accidentally relocate a word twice.
    const bool has_section_relocations = !tables.empty();
    for (const auto &segment : segments)
    {
        if (segment.type == kPspRelocations)
        {
            if (has_section_relocations)
            {
                throw std::invalid_argument("PRX contains both section and segment relocation tables");
            }
            tables.push_back({segment.file_offset, segment.file_size});
        }
    }
    for (const auto &table : tables)
    {
        if (table.size % kRelocationSize != 0)
        {
            throw std::invalid_argument("Truncated PSP relocation record");
        }
        file.require_range(table.offset, table.size);
    }
    return tables;
}

PrxRelocationType relocation_type(std::uint32_t raw)
{
    const auto type = static_cast<PrxRelocationType>(raw);
    switch (type)
    {
    case PrxRelocationType::Mips16:
    case PrxRelocationType::Mips32:
    case PrxRelocationType::Mips26:
    case PrxRelocationType::MipsHigh16:
    case PrxRelocationType::MipsLow16:
    case PrxRelocationType::MipsGpRelative16:
        return type;
    }
    throw std::invalid_argument("Unsupported PSP relocation type");
}

// Decode records once; each table keeps its own HI16/LO16 pairing scope.
std::vector<PrxRelocationTable> read_relocations(PayloadReader file, std::span<const ProgramHeader> segments)
{
    const auto ranges = read_relocation_tables(file, segments);
    std::vector<PrxRelocationTable> tables;
    for (const auto &range : ranges)
    {
        file.seek(range.offset);
        PrxRelocationTable table;
        for (std::size_t index = 0; index < range.size / kRelocationSize; ++index)
        {
            const auto offset = file.read_u32();
            const auto info = file.read_u32();
            const auto type = info & 0xFFU;
            if (type == 0) // R_MIPS_NONE carries no patch.
            {
                continue;
            }
            // PSP r_info uses program-header indices, not ELF symbol-table indices:
            // bits 7-0 select the relocation, 15-8 the segment containing the patch,
            // and 23-16 the segment whose loaded address is added to the stored value.
            const auto patch_segment = (info >> 8) & 0xFFU;
            const auto base_segment = (info >> 16) & 0xFFU;
            if ((info >> 24) != 0 || patch_segment >= segments.size() || base_segment >= segments.size() ||
                segments[patch_segment].type != kPrxLoadSegment || segments[base_segment].type != kPrxLoadSegment)
            {
                throw std::invalid_argument("Invalid PSP relocation segment index");
            }
            table.relocations.push_back({offset, relocation_type(type), patch_segment, base_segment});
        }
        tables.push_back(std::move(table));
    }
    return tables;
}

} // namespace

ParsedPrx read_prx(PayloadSpan bytes)
{
    PayloadReader file(bytes, std::endian::little);
    file.require_range(0, kElfHeaderSize);
    if (file.read_u32() != kElfMagic ||            // e_ident: ELF signature
        file.read_u8() != kElfClass32 ||           // EI_CLASS: 32-bit objects
        file.read_u8() != kLittleEndianEncoding || // EI_DATA: least significant byte first
        file.read_u8() != kElfVersion)             // EI_VERSION: identification version
    {
        throw std::invalid_argument("Expected a plain little-endian ELF32 MIPS PSP PRX");
    }
    file.skip(9);                          // Remaining e_ident bytes after magic, class, data, and version.
    if (file.read_u16() != kPrxType ||     // e_type: PSP PRX
        file.read_u16() != kMipsMachine || // e_machine: MIPS
        file.read_u32() != kElfVersion)    // e_version: object format version
    {
        throw std::invalid_argument("Expected a plain little-endian ELF32 MIPS PSP PRX");
    }
    const auto entry_offset = file.read_u32(); // e_entry
    file.skip(12);                             // Program/section table offsets and flags.
    if (file.read_u16() != kElfHeaderSize)     // e_ehsize: ELF32 header size
    {
        throw std::invalid_argument("Expected a plain little-endian ELF32 MIPS PSP PRX");
    }
    const auto headers = parse_segments(file);
    auto relocations = read_relocations(file, headers);
    const auto &first = headers.front();
    // PSP p_paddr identifies module information by file offset; its top bit marks
    // a kernel module. Convert it once to an offset within the first load segment.
    const auto module_file_offset = first.module_offset & kModuleOffsetMask;
    if (module_file_offset < first.file_offset || module_file_offset - first.file_offset > first.file_size ||
        kPrxModuleInfoSize > first.file_size - (module_file_offset - first.file_offset))
    {
        throw std::invalid_argument("PRX module information is outside the first segment");
    }
    const auto module_offset = module_file_offset - first.file_offset;
    std::vector<PrxSegment> segments;
    for (const auto &header : headers)
    {
        PrxSegment segment{header.type, header.virtual_address, header.memory_size, header.flags, header.alignment, {}};
        if (header.type == kPrxLoadSegment)
        {
            file.seek(header.file_offset);
            const auto contents = file.read_bytes(header.file_size);
            segment.bytes.assign(contents.begin(), contents.end());
        }
        // Non-load entries retain their index but need no bytes after relocation decoding.
        segments.push_back(std::move(segment));
    }
    return {entry_offset, module_offset, std::move(segments), std::move(relocations)};
}

} // namespace psp
