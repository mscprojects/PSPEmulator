#pragma once

#include "loader/prx_reader.hpp"
#include "memory/address.hpp"
#include "runtime/controller_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

enum class ExecutionEvent : std::uint8_t
{
    Vblank,
    Finished,
};

namespace detail
{

class Runtime;

} // namespace detail

// Owns an independent execution. Advance runs to the next vblank or termination,
// including vblanks while all threads sleep or interrupts are masked.
class Execution
{
public:
    explicit Execution(const ParsedPrx &prx, const ExecutionOptions &options = {});
    ~Execution();
    Execution(const Execution &) = delete;
    Execution &operator=(const Execution &) = delete;

    ExecutionEvent advance();
    // Host state is sampled only at a future guest vblank, never on host time.
    void set_controller(ControllerState input);
    // Notify the registered guest exit callback; does not force termination.
    void request_exit();
    std::uint64_t guest_time() const;
    // Packed 480x272 RGBA bytes, captured before returning from advance().
    // At termination only the active framebuffer is captured, not a pending selection.
    std::span<const std::uint8_t> pixels() const;
    // Available after Finished; remains valid until this execution is destroyed.
    const ExecutionResult &result() const;
    // Captured output is also available before termination or after frontend closure.
    std::string_view output() const;

private:
    std::unique_ptr<detail::Runtime> runtime_;
};

// Execute from the PRX entry point with an initialized stack, GP, and argument block.
// Captures console output without accessing host files. Uses a minimal
// PSP runtime, not a complete kernel. Uncalled imports may remain unsupported;
// invoking one, a CPU fault, or budget exhaustion throws with guest PC context.
ExecutionResult execute_prx(const ParsedPrx &prx, const ExecutionOptions &options = {});

} // namespace psp
