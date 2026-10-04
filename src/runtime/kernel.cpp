#include "runtime/kernel.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace psp::detail
{

Kernel::Kernel(Memory &memory, GuestAddress load_address, std::size_t memory_size, std::span<const PrxSegment> segments,
               GuestAddress global_pointer)
    : memory_(memory), heap_(load_address.value_of()),
      stack_top_((static_cast<std::uint64_t>(load_address.value_of()) + memory_size) & ~std::uint64_t{255}),
      global_pointer_(global_pointer), return_address_(GuestAddress{0})
{
    for (const auto &segment : segments)
    {
        if (segment.type == 1)
        {
            heap_ = std::max(heap_, static_cast<std::uint64_t>(load_address.value_of()) + segment.virtual_address +
                                        segment.memory_size);
        }
    }
    heap_ = (heap_ + 255) & ~std::uint64_t{255};
    return_address_ = allocate_memory(256, AllocationDirection::Low);
}

void Kernel::initialize(GuestAddress entry, std::span<const std::string> arguments)
{
    constexpr std::uint32_t stack_size = 0x10000;
    Payload bytes;
    for (const auto &text : arguments)
    {
        if (text.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("Program arguments cannot contain NUL bytes");
        }
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);
    }
    if (bytes.size() > stack_size - 256)
    {
        throw std::invalid_argument("Program arguments exceed startup stack capacity");
    }
    current_thread_ = create_thread({entry, stack_size, 0x20, "startup", 0x80000000});
    auto &initial = threads_.at(current_thread_);
    place_arguments(initial, bytes);
    initial.lifecycle = Lifecycle::Started;
}

std::uint32_t Kernel::create_thread(ThreadCreation creation)
{
    if (creation.stack_size < 512)
    {
        throw std::runtime_error("Thread stack must contain at least 512 bytes");
    }
    const auto stack = allocate_memory(creation.stack_size, AllocationDirection::High);
    CpuState state{.program_counter = creation.entry};
    state.registers[28] = global_pointer_.value_of();
    state.registers[31] = return_address_.value_of();
    const auto id = next_id_++;
    threads_.emplace(id, Thread{std::move(creation), state, stack});
    return id;
}

void Kernel::start_thread(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size)
{
    auto &thread = threads_.at(id);
    if (thread.lifecycle != Lifecycle::Created)
    {
        throw std::runtime_error("Thread already started");
    }
    const auto bytes = memory_.read_bytes(arguments, argument_size);
    if (bytes.size() > thread.creation.stack_size - 256)
    {
        throw std::runtime_error("Thread arguments exceed stack capacity");
    }
    place_arguments(thread, bytes);
    thread.lifecycle = Lifecycle::Started;
    ready_.push_back(id);
}

bool Kernel::select_next_thread()
{
    if (exit_code_)
    {
        return false;
    }
    auto &thread = threads_.at(current_thread_);
    if (thread.state.program_counter == return_address_ || thread.lifecycle == Lifecycle::Finished)
    {
        thread.lifecycle = Lifecycle::Finished;
        if (ready_.empty())
        {
            exit_code_ = std::bit_cast<std::int32_t>(thread.state.registers[2]);
            return false;
        }
        current_thread_ = ready_.front();
        ready_.pop_front();
    }
    return true;
}

CpuState &Kernel::current_thread_state()
{
    return threads_.at(current_thread_).state;
}

std::uint32_t Kernel::current_thread_id() const
{
    return current_thread_;
}

std::uint32_t Kernel::current_thread_priority() const
{
    return threads_.at(current_thread_).creation.priority;
}

void Kernel::exit_thread()
{
    threads_.at(current_thread_).lifecycle = Lifecycle::Finished;
}

void Kernel::exit_game()
{
    exit_code_ = 0;
}

int Kernel::exit_code() const
{
    if (!exit_code_)
    {
        throw std::logic_error("Execution has not finished");
    }
    return *exit_code_;
}

GuestThreadInfo Kernel::thread_status(std::uint32_t id) const
{
    if (id == 0)
    {
        id = current_thread_;
    }
    const auto &thread = threads_.at(id);
    const auto &creation = thread.creation;
    GuestThreadInfo info;
    const auto name_size = static_cast<std::ptrdiff_t>(std::min(creation.name.size(), info.name.size() - 1));
    std::ranges::copy_n(creation.name.begin(), name_size, info.name.begin());
    info.attributes = creation.attributes;
    auto status = ThreadStatus::Stopped;
    if (thread.lifecycle == Lifecycle::Started)
    {
        status = id == current_thread_ ? ThreadStatus::Running : ThreadStatus::Ready;
    }
    info.status = static_cast<std::uint32_t>(status);
    info.entry = creation.entry.value_of();
    info.stack = thread.stack.value_of();
    info.stack_size = creation.stack_size;
    info.global_pointer = global_pointer_.value_of();
    info.initial_priority = creation.priority;
    info.current_priority = creation.priority;
    info.exit_status = thread.lifecycle == Lifecycle::Finished ? thread.state.registers[2] : 0;
    // Wait and scheduling counters remain zero in this cooperative runtime.
    return info;
}

void Kernel::create_mutex(const MutexCreation &creation)
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
    work_area.owner = creation.initial_count == 0 ? 0 : current_thread_;
    work_area.attributes = creation.attributes;
    work_area.id = next_id_;
    write_mutex_work_area(memory_, creation.work_area, work_area);
    mutexes_.emplace(next_id_++, creation.work_area);
}

void Kernel::delete_mutex(GuestAddress address)
{
    auto work_area = mutex_work_area(address);
    const auto id = work_area.id.value();
    work_area.lock_count = 0;
    work_area.owner = 0xFFFFFFFF;
    work_area.id = 0xFFFFFFFF;
    write_mutex_work_area(memory_, address, work_area);
    mutexes_.erase(id);
}

void Kernel::lock_mutex(GuestAddress address, std::uint32_t count, GuestAddress timeout)
{
    auto work_area = mutex_work_area(address);
    validate_mutex_count(work_area, count);
    const auto locked = work_area.lock_count.value();
    if (timeout.value_of() != 0 || locked > 0x7FFFFFFFU || count > 0x7FFFFFFFU - locked ||
        (locked != 0 && (work_area.owner.value() != current_thread_ || (work_area.attributes.value() & 0x200U) == 0)))
    {
        throw std::runtime_error("Unsupported lightweight mutex wait or invalid lock count");
    }
    work_area.lock_count = locked + count;
    work_area.owner = current_thread_;
    write_mutex_work_area(memory_, address, work_area);
}

void Kernel::unlock_mutex(GuestAddress address, std::uint32_t count)
{
    auto work_area = mutex_work_area(address);
    validate_mutex_count(work_area, count);
    const auto locked = work_area.lock_count.value();
    if (work_area.owner.value() != current_thread_ || count > locked)
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

std::uint32_t Kernel::create_semaphore(const SemaphoreCreation &creation)
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

void Kernel::delete_semaphore(std::uint32_t id)
{
    if (semaphores_.erase(id) == 0)
    {
        throw std::runtime_error("Invalid semaphore");
    }
}

// Semaphore identity and requested token count are separate PSP parameters.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Kernel::wait_semaphore(std::uint32_t id, std::uint32_t count, GuestAddress timeout)
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
void Kernel::signal_semaphore(std::uint32_t id, std::uint32_t count)
{
    auto &semaphore = semaphores_.at(id);
    if (count == 0 || count > semaphore.maximum - semaphore.count)
    {
        throw std::runtime_error("Invalid semaphore signal count");
    }
    semaphore.count += count;
}

std::uint32_t Kernel::allocate_partition(PartitionAllocation allocation)
{
    if (allocation.partition != 2 || allocation.type > 1)
    {
        throw std::runtime_error("Unsupported partition allocation");
    }
    const auto direction = allocation.type == 1 ? AllocationDirection::High : AllocationDirection::Low;
    const auto address = allocate_memory(allocation.size, direction);
    const auto id = next_id_++;
    blocks_.emplace(id, address);
    return id;
}

std::uint32_t Kernel::free_memory_size() const
{
    return static_cast<std::uint32_t>(stack_top_ - heap_);
}

GuestAddress Kernel::block_address(std::uint32_t id) const
{
    return blocks_.at(id);
}

GuestAddress Kernel::allocate_memory(std::uint32_t size, AllocationDirection direction)
{
    const auto aligned_size = (static_cast<std::uint64_t>(size) + 255) & ~std::uint64_t{255};
    if (size == 0 || heap_ > stack_top_ || aligned_size > stack_top_ - heap_)
    {
        throw std::runtime_error("Guest memory exhausted");
    }
    if (direction == AllocationDirection::High)
    {
        stack_top_ -= aligned_size;
        return GuestAddress{static_cast<std::uint32_t>(stack_top_)};
    }
    const auto address = GuestAddress{static_cast<std::uint32_t>(heap_)};
    heap_ += aligned_size;
    return address;
}

void Kernel::place_arguments(Thread &thread, PayloadSpan arguments)
{
    // Arguments sit at the top of the stack, above an aligned initial call frame.
    const auto address =
        thread.stack.value_of() + thread.creation.stack_size - static_cast<std::uint32_t>(arguments.size());
    memory_.write_bytes(GuestAddress{address}, arguments);
    thread.state.registers[4] = static_cast<std::uint32_t>(arguments.size());
    thread.state.registers[5] = address;
    thread.state.registers[29] = (address & ~15U) - 64;
}

GuestMutexWorkArea Kernel::mutex_work_area(GuestAddress address) const
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

void Kernel::validate_mutex_count(const GuestMutexWorkArea &work_area, std::uint32_t count) const
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
