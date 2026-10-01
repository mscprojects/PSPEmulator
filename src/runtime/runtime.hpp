#pragma once

#include "runtime/execution.hpp"

#include "cpu/cpu.hpp"
#include "loader/prx.hpp"

#include <deque>
#include <map>
#include <memory>
#include <optional>

namespace psp::detail
{

// Owns all state for one execution. Threads run cooperatively; services outside
// this small headless runtime fail explicitly rather than pretending to succeed.
class Runtime
{
    struct ImportBinding
    {
        std::string library;
        std::uint32_t nid;
    };

    struct Thread
    {
        std::unique_ptr<Cpu> cpu;
        std::uint32_t stack;
        std::uint32_t stack_size;
        std::uint32_t priority;
        bool started{};
        bool finished{};
    };

public:
    Runtime(const ParsedPrx &prx, const ExecutionOptions &options);
    ExecutionResult run();

private:
    std::uint32_t allocate(std::uint32_t size, bool high);
    std::uint32_t create_thread(GuestAddress entry, std::uint32_t stack_size, std::uint32_t priority);
    std::uint32_t argument(const Cpu &cpu, std::size_t index) const;
    std::string read_string(std::uint32_t address) const;
    std::string read_bytes(std::uint32_t address, std::uint32_t size) const;
    std::uint32_t service(Cpu &cpu, const ImportBinding &binding);

    LoadedPrx loaded_;
    const ExecutionOptions &options_;
    std::uint64_t heap_;
    std::uint64_t stack_top_;
    std::uint32_t return_address_;
    std::map<std::uint32_t, ImportBinding> imports_;
    std::map<std::uint32_t, Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::map<std::uint32_t, std::uint32_t> blocks_;
    std::uint32_t next_id_{1};
    std::uint32_t current_thread_{};
    std::optional<int> exit_code_;
    std::string output_;
    std::uint64_t instructions_{};
};

} // namespace psp::detail
