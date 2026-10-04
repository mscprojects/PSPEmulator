#pragma once

#include "runtime/execution.hpp"
#include "runtime/syscall_dispatcher.hpp"

namespace psp::detail
{

// Owns one execution and coordinates the CPU, kernel, syscall dispatch, and
// guest I/O. Those components do not receive a Runtime reference.
class Runtime
{
public:
    Runtime(const ParsedPrx &prx, const ExecutionOptions &options);
    ExecutionResult run();

private:
    LoadedPrx loaded_;
    Cpu cpu_;
    Kernel kernel_;
    GuestIo io_;
    SyscallDispatcher dispatcher_;
    std::uint64_t instruction_budget_;
    std::uint64_t instructions_{};
};

} // namespace psp::detail
