#pragma once

#include "runtime/guest_structures.hpp"

#include <map>

namespace psp::detail
{

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

struct EventFlagCreation
{
    GuestAddress name;
    std::uint32_t attributes;
    std::uint32_t bits;
    GuestAddress options;
};

// Owns synchronization object identities and state. Guest mutex work areas remain
// authoritative; callers supply the current thread for ownership checks.
// Contended waits and timeouts remain unsupported.
class Synchronization
{
    struct Semaphore
    {
        std::uint32_t count;
        std::uint32_t maximum;
    };

public:
    // next_id is shared with threads, callbacks, and partition blocks.
    Synchronization(Memory &memory, std::uint32_t &next_id);
    Synchronization(const Synchronization &) = delete;
    Synchronization &operator=(const Synchronization &) = delete;
    void create_mutex(const MutexCreation &creation, std::uint32_t thread_id);
    void delete_mutex(GuestAddress address);
    void lock_mutex(GuestAddress address, std::uint32_t count, GuestAddress timeout, std::uint32_t thread_id);
    void unlock_mutex(GuestAddress address, std::uint32_t count, std::uint32_t thread_id);
    std::uint32_t create_semaphore(const SemaphoreCreation &creation);
    void delete_semaphore(std::uint32_t id);
    void wait_semaphore(std::uint32_t id, std::uint32_t count, GuestAddress timeout);
    void signal_semaphore(std::uint32_t id, std::uint32_t count);
    std::uint32_t create_event_flag(const EventFlagCreation &creation);
    void delete_event_flag(std::uint32_t id);

private:
    GuestMutexWorkArea mutex_work_area(GuestAddress address) const;
    void validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const;

    Memory &memory_;
    std::uint32_t &next_id_;
    std::map<std::uint32_t, GuestAddress> mutexes_;
    std::map<std::uint32_t, Semaphore> semaphores_;
    std::map<std::uint32_t, std::uint32_t> event_flags_;
};

} // namespace psp::detail
