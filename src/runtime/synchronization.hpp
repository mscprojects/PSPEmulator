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

// Owns synchronization identity and count rules. Blocking waits, callbacks, and
// timeouts remain unsupported; failures never masquerade as completed waits.
class Synchronization
{
    struct Semaphore
    {
        std::uint32_t count;
        std::uint32_t maximum;
    };

public:
    Synchronization(Memory &memory, std::uint32_t &next_id);
    void create_mutex(const MutexCreation &creation, std::uint32_t thread_id);
    void delete_mutex(GuestAddress address);
    void lock_mutex(GuestAddress address, std::uint32_t count, GuestAddress timeout, std::uint32_t thread_id);
    void unlock_mutex(GuestAddress address, std::uint32_t count, std::uint32_t thread_id);
    std::uint32_t create_semaphore(const SemaphoreCreation &creation);
    void delete_semaphore(std::uint32_t id);
    void wait_semaphore(std::uint32_t id, std::uint32_t count, GuestAddress timeout);
    void signal_semaphore(std::uint32_t id, std::uint32_t count);

private:
    GuestMutexWorkArea mutex_work_area(GuestAddress address) const;
    void validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const;

    Memory &memory_;
    std::uint32_t &next_id_;
    std::map<std::uint32_t, GuestAddress> mutexes_;
    std::map<std::uint32_t, Semaphore> semaphores_;
};

} // namespace psp::detail
