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
    ExecutionEvent advance();
    // True when guest work cannot proceed without host input, such as an exit request.
    bool blocked() const;
    void set_controller(ControllerState input);
    void request_exit();
    std::uint64_t guest_time() const;
    std::span<const std::uint8_t> pixels();
    const ExecutionResult &result() const;
    // Captured output is also available before termination or after frontend closure.
    std::string_view output() const;

private:
    LoadedPrx loaded_;
    Cpu cpu_;
    Kernel kernel_;
    GuestIo io_;
    Display display_;
    Controller controller_;
    Ge ge_;
    SyscallDispatcher dispatcher_;
    std::uint64_t instruction_budget_;
    std::uint64_t instructions_{};
    bool frame_captured_{};
    std::optional<ExecutionResult> result_;
};

} // namespace psp::detail
