#pragma once

#include "common/payload.hpp"
#include "cpu/cpu_state.hpp"
#include "memory/memory.hpp"
#include "runtime/address_arena.hpp"
#include "runtime/guest_structures.hpp"
#include "runtime/lcd.hpp"

#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

enum class ThreadSelection : std::uint8_t
{
    Ready,
    Idle,
    Finished,
};

// Owns threads, scheduling, the guest clock, interrupts, and callbacks.
// Delays use guest microseconds. Controller waits and callback-enabled sleep
// are supported. Higher-priority ready threads preempt at instruction boundaries;
// equal-priority time slicing remains unsupported.
class Kernel
{
public:
    // Why a thread is blocked. Delays, vblank waits, and callback-enabled sleep have
    // their own services; other waits use wait() and end through wake(). Switches over
    // Wait have no default, so the compiler flags each place a new reason must be handled.
    enum class Wait : std::uint8_t
    {
        None,
        SleepCallback,
        Delay,
        Controller,
        Ge,
        Vblank,
    };

private:
    enum class Lifecycle : std::uint8_t
    {
        Created,
        Started,
        Waiting,
        Finished,
    };

    struct CallbackContext
    {
        CpuState state;
        std::optional<std::uint32_t> id;
        Lifecycle lifecycle;
        Wait wait;
        bool interrupts_enabled;
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

public:
    // The arena and next_id are shared with the other runtime services.
    // Thread stacks and the return sentinel reserve ranges for the execution lifetime.
    Kernel(Memory &memory, AddressArena &arena, std::uint32_t &next_id, GuestAddress global_pointer);
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;
    void initialize(GuestAddress entry, std::span<const std::string> arguments);
    std::uint32_t create_thread(ThreadCreation creation);
    void start_thread(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size);
    // Wake elapsed delays, complete returning/exited threads, and select a ready
    // thread by priority, with FIFO ties. Higher-priority ready threads preempt.
    // Idle means waiting threads remain with no ready thread; selection never advances
    // guest time. No thread switch occurs inside Cpu::step().
    ThreadSelection select_next_thread();
    CpuState &current_thread_state();
    std::uint32_t current_thread_id() const;
    std::uint32_t current_thread_priority() const;
    // Delay the current thread; v0 is supplied when the delay expires. A zero
    // delay yields to ready threads of equal priority without advancing guest time.
    void delay_thread(std::uint32_t microseconds);
    // Block the current thread until wake() ends this wait. Accepts Controller and Ge.
    void wait(Wait reason);
    // End a thread's wait for reason and supply v0. A thread interrupted by a GE callback
    // resumes its saved continuation only after the callback returns.
    void wake(std::uint32_t id, Wait reason, std::uint32_t result);
    // Delay the current thread until the next vblank edge, reported as a vblank wait.
    void wait_vblank();
    std::uint32_t create_callback(GuestAddress entry, GuestAddress common);
    void register_exit_callback(std::uint32_t id);
    void sleep_thread_callbacks();
    // Queue a notification; guest code runs only in its owner's callback-enabled sleep.
    // An early request is retained until an exit callback is registered.
    void request_exit();
    bool interrupt_callback_ready() const;
    // True when no thread can run or wake through guest time: every live thread waits
    // for a callback notification or GE completion, and no notification is pending.
    bool untimed_waits_only() const;
    // Run a GE interrupt callback on the selected thread, preserving an idle wait.
    // Interrupt callbacks cannot block or be preempted by another thread. Returns false
    // while interrupts are masked, any callback is active, or no live thread remains.
    // Callers retain deferred notifications.
    bool enter_interrupt_callback(GuestAddress entry, std::uint32_t argument, GuestAddress common);
    // Runtime currently advances one microsecond per instruction, a provisional
    // deterministic rate rather than a cycle-accurate CPU clock. Returns true when the
    // clock reaches an LCD vblank edge, which also marks the vblank interrupt pending.
    bool advance_time(std::uint64_t microseconds);
    std::uint64_t system_time() const;
    // First integer microsecond at or after the next LCD edge; throws at clock overflow.
    std::uint64_t next_vblank_time() const;
    // Earliest delay or vblank-wait deadline, if any thread waits on guest time.
    std::optional<std::uint64_t> next_wakeup_time() const;
    // Handle a pending periodic vblank interrupt at an instruction boundary.
    // The HLE handler preserves CPU state except for the Allegrex link bit.
    // Masked events coalesce and remain pending until interrupts are enabled.
    bool deliver_pending_interrupt();
    std::uint32_t suspend_interrupts();
    void resume_interrupts(std::uint32_t flags);
    bool interrupts_enabled() const;
    void exit_thread();
    // Stop another ready thread or cancel its timed wait. Stopped threads can be restarted.
    // Callback and device-wait cancellation remain unsupported.
    std::uint32_t terminate_thread(std::uint32_t id);
    void exit_game();
    int exit_code() const;
    GuestThreadInfo thread_status(std::uint32_t id) const;

private:
    void place_arguments(Thread &thread, PayloadSpan arguments);
    // Handle the current thread's return to a sentinel after its delay slot: restore the
    // context saved before a callback, or finish the thread.
    void complete_return(Thread &thread);
    void delay_until(std::uint64_t deadline, Wait reason);
    void wake_delayed_threads();
    void wake_callbacks();
    // Only a running thread outside a GE interrupt callback can block.
    void block_current_thread(Wait reason);
    void begin_callback(Thread &thread, GuestAddress entry, std::array<std::uint32_t, 3> arguments,
                        std::optional<std::uint32_t> id);

    Memory &memory_;
    // Thread stacks and partition blocks share one arena; only partition blocks can be freed.
    AddressArena &arena_;
    std::uint32_t &next_id_;
    GuestAddress global_pointer_;
    GuestAddress return_address_;
    std::map<std::uint32_t, Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::uint32_t current_thread_{};
    std::uint64_t system_time_{};
    // Cached so instruction steps avoid division. Empty once the next edge exceeds the clock range.
    std::optional<std::uint64_t> next_vblank_;
    std::multimap<std::uint64_t, std::uint32_t> delayed_;
    std::map<std::uint32_t, Callback> callbacks_;
    std::optional<std::uint32_t> exit_callback_;
    bool exit_requested_{};
    bool interrupts_enabled_{true};
    bool interrupt_pending_{};
    std::optional<int> exit_code_;
};

} // namespace psp::detail
