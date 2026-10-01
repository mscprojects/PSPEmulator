#pragma once

#include "loader/prx_reader.hpp"
#include "memory/address.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace psp
{

struct ExecutionOptions
{
    GuestAddress load_address{0x08800000};
    std::size_t memory_size{0x01800000};
    std::uint64_t max_instructions{50'000'000};
    std::vector<std::string> arguments{"program.prx"};
};

struct ExecutionResult
{
    std::string output;
    int exit_code;
    std::uint64_t instructions_executed;
};

// Execute from the PRX entry point with an initialized stack, GP, and argument block.
// Captures console output without accessing host files. Uses a minimal cooperative
// PSP runtime, not a complete kernel. Uncalled imports may remain unsupported;
// invoking one, a CPU fault, or budget exhaustion throws with guest PC context.
ExecutionResult execute_prx(const ParsedPrx &prx, const ExecutionOptions &options = {});

} // namespace psp
