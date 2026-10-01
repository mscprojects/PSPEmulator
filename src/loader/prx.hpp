#pragma once

#include "loader/prx_reader.hpp"
#include "memory/address.hpp"
#include "memory/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace psp
{

struct PrxFunctionImport
{
    // Library function identifier used for resolution; this is not a guest address.
    std::uint32_t nid;
    // Guest address of the instruction stub; loading leaves resolution to a later stage.
    GuestAddress stub_address;
};

struct PrxImportLibrary
{
    std::string name;
    std::uint16_t version;
    std::uint16_t attributes;
    std::vector<PrxFunctionImport> functions;
};

struct PrxModule
{
    std::string name;
    std::uint16_t attributes;
    std::uint16_t version;
    GuestAddress global_pointer;
    GuestAddress exports_begin;
    GuestAddress exports_end;
};

struct LoadedPrx
{
    Memory memory;
    GuestAddress entry_point;
    PrxModule module;
    std::vector<PrxImportLibrary> imports;
};

// Prepare a parsed PRX in zeroed memory starting at load_address.
// memory_size includes space for the image and any later stack/heap allocations.
// Applies uncompressed PSP relocations and leaves function import stubs untouched.
// Throws std::invalid_argument for malformed or unsupported input; no partial result escapes.
LoadedPrx prepare_prx(const ParsedPrx &prx, GuestAddress load_address, std::size_t memory_size);

} // namespace psp
