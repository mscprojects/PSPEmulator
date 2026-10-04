#pragma once

#include "cpu/cpu.hpp"
#include "loader/prx.hpp"
#include "runtime/guest_io.hpp"
#include "runtime/kernel.hpp"

#include <map>
#include <span>

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
    SyscallDispatcher(Memory &memory, Kernel &kernel, GuestAllocator &allocator, GuestIo &io,
                      std::span<const PrxImportLibrary> imports);
    // Current services complete synchronously or throw. If blocking waits are
    // introduced, their return register must be supplied on wake-up instead.
    void handle(const Syscall &syscall, CpuState &state, std::uint64_t instructions);

private:
    std::uint32_t dispatch(CpuState &state, const ImportBinding &binding, std::uint64_t instructions);

    Memory &memory_;
    Kernel &kernel_;
    GuestAllocator &allocator_;
    GuestIo &io_;
    std::map<std::uint32_t, ImportBinding> imports_;
};

} // namespace psp::detail
