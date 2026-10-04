#pragma once

#include "runtime/execution.hpp"
#include "runtime/syscall_dispatcher.hpp"

namespace psp::detail
{

// Owns one execution and coordinates its components. Kernel state, allocation,
// import dispatch, and guest I/O have separate owners and no Runtime reference.
class Runtime
{
public:
    Runtime(const ParsedPrx &prx, const ExecutionOptions &options);
    ExecutionResult run();

private:
    LoadedPrx loaded_;
    Cpu cpu_;
    GuestAllocator allocator_;
    Kernel kernel_;
    GuestIo io_;
    SyscallDispatcher dispatcher_;
    std::uint64_t instruction_budget_;
    std::uint64_t instructions_{};
};

} // namespace psp::detail
