#include "runtime/synchronization.hpp"

#include <stdexcept>

namespace psp::detail
{

Synchronization::Synchronization(Memory &memory, std::uint32_t &next_id) : memory_(memory), next_id_(next_id)
{
}

void Synchronization::create_mutex(const MutexCreation &creation, std::uint32_t thread_id)
{
    if ((creation.work_area.value_of() & 3U) != 0 || creation.options.value_of() != 0 ||
        (creation.attributes & ~0x300U) != 0 || creation.initial_count > 0x7FFFFFFFU ||
        ((creation.attributes & 0x200U) == 0 && creation.initial_count > 1))
    {
        throw std::runtime_error("Unsupported lightweight mutex creation parameters");
    }
    memory_.read_c_string(creation.name, 32);
    GuestMutexWorkArea work_area;
    work_area.lock_count = creation.initial_count;
    work_area.owner = creation.initial_count == 0 ? 0 : thread_id;
    work_area.attributes = creation.attributes;
    work_area.id = next_id_;
    write_mutex_work_area(memory_, creation.work_area, work_area);
    mutexes_.emplace(next_id_++, creation.work_area);
}

void Synchronization::delete_mutex(GuestAddress address)
{
    auto work_area = mutex_work_area(address);
    const auto id = work_area.id.value();
    work_area.lock_count = 0;
    work_area.owner = 0xFFFFFFFF;
    work_area.id = 0xFFFFFFFF;
    write_mutex_work_area(memory_, address, work_area);
    mutexes_.erase(id);
}

void Synchronization::lock_mutex(GuestAddress address, std::uint32_t count, GuestAddress timeout,
                                 std::uint32_t thread_id)
{
    auto work_area = mutex_work_area(address);
    validate_mutex_count(work_area, count);
    const auto locked = work_area.lock_count.value();
    if (timeout.value_of() != 0 || locked > 0x7FFFFFFFU || count > 0x7FFFFFFFU - locked ||
        (locked != 0 && (work_area.owner.value() != thread_id || (work_area.attributes.value() & 0x200U) == 0)))
    {
        throw std::runtime_error("Unsupported lightweight mutex wait or invalid lock count");
    }
    work_area.lock_count = locked + count;
    work_area.owner = thread_id;
    write_mutex_work_area(memory_, address, work_area);
}

void Synchronization::unlock_mutex(GuestAddress address, std::uint32_t count, std::uint32_t thread_id)
{
    auto work_area = mutex_work_area(address);
    validate_mutex_count(work_area, count);
    const auto locked = work_area.lock_count.value();
    if (work_area.owner.value() != thread_id || count > locked)
    {
        throw std::runtime_error("Lightweight mutex is not owned or unlock count exceeds lock count");
    }
    work_area.lock_count = locked - count;
    if (locked == count)
    {
        work_area.owner = 0;
    }
    write_mutex_work_area(memory_, address, work_area);
}

std::uint32_t Synchronization::create_semaphore(const SemaphoreCreation &creation)
{
    if (creation.attributes != 0 || creation.options.value_of() != 0 || creation.maximum == 0 ||
        creation.maximum > 0x7FFFFFFFU || creation.initial_count > creation.maximum)
    {
        throw std::runtime_error("Unsupported semaphore creation parameters");
    }
    memory_.read_c_string(creation.name, 32);
    const auto id = next_id_++;
    semaphores_.emplace(id, Semaphore{creation.initial_count, creation.maximum});
    return id;
}

void Synchronization::delete_semaphore(std::uint32_t id)
{
    if (semaphores_.erase(id) == 0)
    {
        throw std::runtime_error("Invalid semaphore");
    }
}

// Semaphore identity and requested token count are separate PSP parameters.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Synchronization::wait_semaphore(std::uint32_t id, std::uint32_t count, GuestAddress timeout)
{
    auto &semaphore = semaphores_.at(id);
    if (timeout.value_of() != 0 || count == 0 || count > semaphore.count)
    {
        throw std::runtime_error("Unsupported semaphore wait");
    }
    semaphore.count -= count;
}

// Semaphore identity and released token count are separate PSP parameters.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Synchronization::signal_semaphore(std::uint32_t id, std::uint32_t count)
{
    auto &semaphore = semaphores_.at(id);
    if (count == 0 || count > semaphore.maximum - semaphore.count)
    {
        throw std::runtime_error("Invalid semaphore signal count");
    }
    semaphore.count += count;
}

GuestMutexWorkArea Synchronization::mutex_work_area(GuestAddress address) const
{
    if ((address.value_of() & 3U) != 0)
    {
        throw std::runtime_error("Misaligned lightweight mutex work area");
    }
    const auto work_area = read_mutex_work_area(memory_, address);
    const auto mutex = mutexes_.find(work_area.id.value());
    if (mutex == mutexes_.end() || mutex->second != address || work_area.waiter_count.value() != 0)
    {
        throw std::runtime_error("Invalid lightweight mutex or unsupported waiters");
    }
    return work_area;
}

void Synchronization::validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const
{
    if (count == 0 || count > 0x7FFFFFFFU)
    {
        throw std::runtime_error("Invalid lightweight mutex lock count");
    }
    if ((work_area.attributes.value() & 0x200U) == 0 && count != 1)
    {
        throw std::runtime_error("Nonrecursive lightweight mutex requires count one");
    }
}

} // namespace psp::detail
