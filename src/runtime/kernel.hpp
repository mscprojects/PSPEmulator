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

struct MutexCreation
{
    GuestAddress work_area;
    GuestAddress name;
    std::uint32_t attributes;
    std::uint32_t initial_count;
    GuestAddress options;
};

struct SemaphoreCreation
{
    GuestAddress name;
    std::uint32_t attributes;
    std::uint32_t initial_count;
    std::uint32_t maximum;
    GuestAddress options;
};

// Owns threads, cooperative scheduling, synchronization, and kernel object IDs.
// Blocking waits, callbacks, timeouts, and preemption remain unsupported.
class Kernel
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

    struct Semaphore
    {
        std::uint32_t count;
        std::uint32_t maximum;
    };

public:
    Kernel(Memory &memory, GuestAllocator &allocator, GuestAddress global_pointer);
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;
    void initialize(GuestAddress entry, std::span<const std::string> arguments);
    std::uint32_t create_thread(ThreadCreation creation);
    void start_thread(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size);
    // Complete a returning/exited thread and select the next ready one. False
    // means execution has ended; no thread switch occurs inside Cpu::step().
    bool select_next_thread();
    CpuState &current_thread_state();
    std::uint32_t current_thread_id() const;
    std::uint32_t current_thread_priority() const;
    void exit_thread();
    void exit_game();
    int exit_code() const;
    GuestThreadInfo thread_status(std::uint32_t id) const;

    void create_mutex(const MutexCreation &creation);
    void delete_mutex(GuestAddress address);
    void lock_mutex(GuestAddress address, std::uint32_t count, GuestAddress timeout);
    void unlock_mutex(GuestAddress address, std::uint32_t count);
    std::uint32_t create_semaphore(const SemaphoreCreation &creation);
    void delete_semaphore(std::uint32_t id);
    void wait_semaphore(std::uint32_t id, std::uint32_t count, GuestAddress timeout);
    void signal_semaphore(std::uint32_t id, std::uint32_t count);

    std::uint32_t allocate_partition(PartitionAllocation allocation);

private:
    void place_arguments(Thread &thread, PayloadSpan arguments);
    GuestMutexWorkArea mutex_work_area(GuestAddress address) const;
    void validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const;

    Memory &memory_;
    GuestAllocator &allocator_;
    std::uint32_t next_id_{1};
    GuestAddress global_pointer_;
    GuestAddress return_address_;
    std::map<std::uint32_t, Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::uint32_t current_thread_{};
    std::optional<int> exit_code_;
    std::map<std::uint32_t, GuestAddress> mutexes_;
    std::map<std::uint32_t, Semaphore> semaphores_;
};

} // namespace psp::detail
