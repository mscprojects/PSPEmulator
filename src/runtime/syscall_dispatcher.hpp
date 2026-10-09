#pragma once

#include "cpu/cpu.hpp"
#include "loader/prx.hpp"
#include "runtime/display.hpp"
#include "runtime/guest_io.hpp"
#include "runtime/kernel.hpp"

#include <optional>
#include <span>
#include <vector>

namespace psp::detail
{

// Binds PRX import stubs and translates the PSP register ABI into named service
// operations. Services receive their own dependencies, never the Runtime.
class SyscallDispatcher
{
    struct ImportBinding
    {
        std::string library;
        std::uint32_t nid;
    };

public:
    SyscallDispatcher(Memory &memory, Kernel &kernel, GuestIo &io, Display &display,
                      std::span<const PrxImportLibrary> imports);
    // A delayed service leaves v0 untouched until Kernel wakes the thread.
    void handle(const Syscall &syscall, CpuState &state);

private:
    std::optional<std::uint32_t> dispatch(CpuState &state, const ImportBinding &binding);

    Memory &memory_;
    Kernel &kernel_;
    GuestIo &io_;
    Display &display_;
    std::vector<ImportBinding> imports_;
};

} // namespace psp::detail
