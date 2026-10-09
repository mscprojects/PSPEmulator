#pragma once

#include "cpu/cpu_state.hpp"
#include "loader/prx_reader.hpp"
#include "runtime/guest_structures.hpp"

#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psp::detail
{

struct PartitionAllocation
{
    std::uint32_t partition;
    std::uint32_t type;
    std::uint32_t size;
};

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

enum class ThreadSelection : std::uint8_t
{
    Ready,
    Idle,
    Finished,
};

// Owns threads, scheduling, synchronization, allocations, and kernel object IDs.
// Delays use guest microseconds. Controller waits and callback-enabled sleep
// are supported. Higher-priority ready threads preempt at instruction boundaries;
// blocking synchronization and equal-priority time slicing remain unsupported.
class Kernel
{
    enum class AllocationDirection : std::uint8_t
    {
        Low,
        High,
    };

    enum class Lifecycle : std::uint8_t
    {
        Created,
        Started,
        Waiting,
        Finished,
    };

    enum class Wait : std::uint8_t
    {
        None,
        SleepCallback,
        Delay,
        Controller,
    };

    struct CallbackContext
    {
        CpuState state;
        std::uint32_t id;
    };

    struct Thread
    {
        ThreadCreation creation;
        CpuState state;
        GuestAddress stack;
        Lifecycle lifecycle{Lifecycle::Created};
        Wait wait{Wait::None};
        std::optional<CallbackContext> callback{};
    };

    struct Callback
    {
        std::uint32_t owner;
        GuestAddress entry;
        GuestAddress common;
        std::uint32_t notifications{};
    };

    struct Semaphore
    {
        std::uint32_t count;
        std::uint32_t maximum;
    };

    struct MemoryRange
    {
        std::uint64_t begin;
        std::uint64_t end;
    };

public:
    Kernel(Memory &memory, GuestAddress load_address, std::size_t memory_size, std::span<const PrxSegment> segments,
           GuestAddress global_pointer);
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;
    void initialize(GuestAddress entry, std::span<const std::string> arguments);
    std::uint32_t create_thread(ThreadCreation creation);
    void start_thread(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size);
    // Wake elapsed delays, complete returning/exited threads, and select a ready
    // thread by priority, with FIFO ties. Higher-priority ready threads preempt.
    // Idle advancement stops at the earlier of a wakeup and idle_deadline.
    // Idle means waiting threads remain with no ready thread at the deadline.
    // No thread switch occurs inside Cpu::step().
    ThreadSelection select_next_thread(std::uint64_t idle_deadline = std::numeric_limits<std::uint64_t>::max());
    CpuState &current_thread_state();
    std::uint32_t current_thread_id() const;
    std::uint32_t current_thread_priority() const;
    // Delay the current thread; v0 is supplied when the delay expires. A zero
    // delay yields to ready threads of equal priority without advancing guest time.
    void delay_thread(std::uint32_t microseconds);
    void wait_controller();
    void wake_controller(std::uint32_t id);
    std::uint32_t create_callback(GuestAddress entry, GuestAddress common);
    void register_exit_callback(std::uint32_t id);
    void sleep_thread_callbacks();
    // Queue a notification; guest code runs only in its owner's callback-enabled sleep.
    // An early request is retained until an exit callback is registered.
    void request_exit();
    // Runtime currently advances one microsecond per instruction, a provisional
    // deterministic rate rather than a cycle-accurate CPU clock.
    void advance_time(std::uint64_t microseconds);
    std::uint64_t system_time() const;
    // First integer microsecond at or after the next LCD edge; throws at clock overflow.
    std::uint64_t next_vblank_time() const;
    // Handle a pending periodic vblank interrupt at an instruction boundary.
    // The HLE handler preserves CPU state except for the Allegrex link bit.
    // Masked events coalesce and remain pending until interrupts are enabled.
    bool deliver_pending_interrupt();
    std::uint32_t suspend_interrupts();
    void resume_interrupts(std::uint32_t flags);
    bool interrupts_enabled() const;
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
    void free_partition(std::uint32_t id);
    std::uint32_t free_memory_size() const;
    std::uint32_t largest_free_memory_size() const;
    GuestAddress block_address(std::uint32_t id) const;

private:
    // Thread stacks and partition blocks share address-ordered free ranges.
    // Allocations use 256-byte alignment; only partition blocks can be freed.
    MemoryRange allocate_memory(std::uint32_t size, AllocationDirection direction);
    void place_arguments(Thread &thread, PayloadSpan arguments);
    void wake_delayed_threads();
    void wake_callbacks();
    GuestMutexWorkArea mutex_work_area(GuestAddress address) const;
    void validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const;

    Memory &memory_;
    std::vector<MemoryRange> free_ranges_;
    std::map<std::uint32_t, MemoryRange> blocks_;
    std::uint32_t next_id_{1};
    GuestAddress global_pointer_;
    GuestAddress return_address_;
    std::map<std::uint32_t, Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::uint32_t current_thread_{};
    std::uint64_t system_time_{};
    std::multimap<std::uint64_t, std::uint32_t> delayed_;
    std::map<std::uint32_t, Callback> callbacks_;
    std::optional<std::uint32_t> exit_callback_;
    bool exit_requested_{};
    bool interrupts_enabled_{true};
    bool interrupt_pending_{};
    std::optional<int> exit_code_;
    std::map<std::uint32_t, GuestAddress> mutexes_;
    std::map<std::uint32_t, Semaphore> semaphores_;
};

} // namespace psp::detail
