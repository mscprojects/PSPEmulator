#pragma once

#include "cpu/cpu_state.hpp"
#include "runtime/guest_allocator.hpp"
#include "runtime/guest_structures.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>

namespace psp::detail
{

struct ThreadCreation
{
    GuestAddress entry;
    std::uint32_t stack_size;
    std::uint32_t priority;
    std::string name;
    std::uint32_t attributes;
};

// Owns saved CPU states and cooperative scheduling. Only ready and completed
// threads are supported; blocking waits and preemption require further work.
class Threads
{
    enum class Lifecycle : std::uint8_t
    {
        Created,
        Started,
        Finished,
    };

    struct Thread
    {
        ThreadCreation creation;
        CpuState state;
        GuestAddress stack;
        Lifecycle lifecycle{Lifecycle::Created};
    };

public:
    Threads(Memory &memory, GuestAllocator &allocator, std::uint32_t &next_id, GuestAddress global_pointer);
    void initialize(GuestAddress entry, std::span<const std::string> arguments);
    std::uint32_t create(ThreadCreation creation);
    void start(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size);
    // Complete a returning/exited thread and select the next ready one. False
    // means execution has ended; no thread switch occurs inside Cpu::step().
    bool select_next();
    CpuState &current_state();
    std::uint32_t current_id() const;
    std::uint32_t current_priority() const;
    void exit_current();
    void exit_game();
    int exit_code() const;
    GuestThreadInfo status(std::uint32_t id) const;

private:
    void place_arguments(Thread &thread, PayloadSpan arguments);

    Memory &memory_;
    GuestAllocator &allocator_;
    std::uint32_t &next_id_;
    GuestAddress global_pointer_;
    GuestAddress return_address_;
    std::map<std::uint32_t, Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::uint32_t current_thread_{};
    std::optional<int> exit_code_;
};

} // namespace psp::detail
