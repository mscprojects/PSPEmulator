#include "runtime/kernel.hpp"

#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace psp::detail
{

Kernel::Kernel(Memory &memory, GuestAddress load_address, std::size_t memory_size, std::span<const PrxSegment> segments,
               GuestAddress global_pointer)
    : memory_(memory), global_pointer_(global_pointer), return_address_(GuestAddress{0})
{
    if (memory_size == 0 || memory_size > (std::uint64_t{1} << 32) - load_address.value_of())
    {
        throw std::invalid_argument("Kernel memory must fit in the 32-bit address space");
    }
    std::uint64_t begin = load_address.value_of();
    const auto end = (begin + memory_size) & ~std::uint64_t{255};
    for (const auto &segment : segments)
    {
        if (segment.type == 1)
        {
            begin = std::max(begin, static_cast<std::uint64_t>(load_address.value_of()) + segment.virtual_address +
                                        segment.memory_size);
        }
    }
    begin = (begin + 255) & ~std::uint64_t{255};
    if (begin < end)
    {
        free_ranges_.push_back({begin, end});
    }
    return_address_ = GuestAddress{static_cast<std::uint32_t>(allocate_memory(256, AllocationDirection::Low).begin)};
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
    const auto stack =
        GuestAddress{static_cast<std::uint32_t>(allocate_memory(creation.stack_size, AllocationDirection::High).begin)};
    CpuState state{.program_counter = creation.entry};
    state.registers[28] = global_pointer_.value_of();
    state.registers[31] = return_address_.value_of();
    const auto id = next_id_++;
    threads_.emplace(id, Thread{.creation = std::move(creation), .state = state, .stack = stack});
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

ThreadSelection Kernel::select_next_thread(std::uint64_t idle_deadline)
{
    if (idle_deadline < system_time_)
    {
        throw std::invalid_argument("Idle deadline precedes guest time");
    }
    if (exit_code_)
    {
        return ThreadSelection::Finished;
    }
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle == Lifecycle::Started && thread.callback &&
        thread.state.program_counter == GuestAddress{return_address_.value_of() + 4})
    {
        const auto callback_id = thread.callback->id;
        const auto remove = thread.state.registers[2] != 0;
        thread.state = thread.callback->state;
        thread.state.load_linked = false;
        thread.callback.reset();
        thread.lifecycle = callback_id ? Lifecycle::Waiting : Lifecycle::Started;
        thread.wait = callback_id ? Wait::SleepCallback : Wait::None;
        if (remove && callback_id)
        {
            callbacks_.erase(*callback_id);
            if (exit_callback_ == callback_id)
            {
                exit_callback_.reset();
            }
        }
    }
    if (thread.lifecycle == Lifecycle::Started && thread.state.program_counter == return_address_)
    {
        thread.lifecycle = Lifecycle::Finished;
    }
    const bool select_ready = thread.lifecycle != Lifecycle::Started;
    wake_delayed_threads();
    wake_callbacks();
    if (select_ready)
    {
        if (ready_.empty() &&
            std::ranges::any_of(threads_, [](const auto &item) { return item.second.lifecycle == Lifecycle::Waiting; }))
        {
            const auto deadline = delayed_.empty() ? idle_deadline : std::min(delayed_.begin()->first, idle_deadline);
            advance_time(deadline - system_time_);
            wake_delayed_threads();
        }
        if (ready_.empty() &&
            std::ranges::any_of(threads_, [](const auto &item) { return item.second.lifecycle == Lifecycle::Waiting; }))
        {
            return ThreadSelection::Idle;
        }
        if (ready_.empty())
        {
            exit_code_ = std::bit_cast<std::int32_t>(thread.state.registers[2]);
            return ThreadSelection::Finished;
        }
    }
    if (!ready_.empty())
    {
        const auto next =
            std::ranges::min_element(ready_, {}, [&](std::uint32_t id) { return threads_.at(id).creation.priority; });
        if (select_ready || threads_.at(*next).creation.priority < thread.creation.priority)
        {
            const auto id = *next;
            ready_.erase(next);
            if (!select_ready)
            {
                thread.state.load_linked = false;
                ready_.push_back(current_thread_);
            }
            current_thread_ = id;
        }
    }
    return ThreadSelection::Ready;
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

void Kernel::delay_thread(std::uint32_t microseconds)
{
    if (microseconds > std::numeric_limits<std::uint64_t>::max() - system_time_)
    {
        throw std::overflow_error("Guest delay exceeds clock range");
    }
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        throw std::logic_error("Only a running thread can delay");
    }
    delayed_.emplace(system_time_ + microseconds, current_thread_);
    thread.lifecycle = Lifecycle::Waiting;
    thread.wait = Wait::Delay;
}

void Kernel::wait_controller()
{
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        throw std::logic_error("Only a running thread can wait for a controller sample");
    }
    thread.lifecycle = Lifecycle::Waiting;
    thread.wait = Wait::Controller;
}

void Kernel::wake_controller(std::uint32_t id)
{
    auto &thread = threads_.at(id);
    if (thread.lifecycle != Lifecycle::Waiting || thread.wait != Wait::Controller)
    {
        throw std::logic_error("Thread is not waiting for a controller sample");
    }
    thread.state.registers[2] = 1;
    thread.lifecycle = Lifecycle::Started;
    thread.wait = Wait::None;
    ready_.push_back(id);
}

void Kernel::wait_ge()
{
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        throw std::logic_error("Only a running thread can wait for the GE");
    }
    thread.lifecycle = Lifecycle::Waiting;
    thread.wait = Wait::Ge;
}

void Kernel::wake_ge(std::uint32_t id)
{
    auto &thread = threads_.at(id);
    if (thread.lifecycle != Lifecycle::Waiting || thread.wait != Wait::Ge)
    {
        throw std::logic_error("Thread is not waiting for the GE");
    }
    thread.state.registers[2] = 0;
    thread.lifecycle = Lifecycle::Started;
    thread.wait = Wait::None;
    ready_.push_back(id);
}

void Kernel::wait_vblank()
{
    delay_thread(static_cast<std::uint32_t>(next_vblank_time() - system_time_));
    threads_.at(current_thread_).wait = Wait::Vblank;
}

std::uint32_t Kernel::create_callback(GuestAddress entry, GuestAddress common)
{
    if ((entry.value_of() & 3U) != 0 || entry.value_of() == 0)
    {
        throw std::runtime_error("Callback entry must be non-null and aligned");
    }
    memory_.validate_range(entry, 4);
    const auto id = next_id_++;
    callbacks_.emplace(id, Callback{current_thread_, entry, common});
    return id;
}

void Kernel::register_exit_callback(std::uint32_t id)
{
    callbacks_.at(id);
    exit_callback_ = id;
    if (exit_requested_)
    {
        request_exit();
    }
}

void Kernel::sleep_thread_callbacks()
{
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        throw std::logic_error("Only a running thread can sleep");
    }
    if (thread.callback)
    {
        throw std::runtime_error("Nested callback-enabled sleep is unsupported");
    }
    thread.lifecycle = Lifecycle::Waiting;
    thread.wait = Wait::SleepCallback;
}

void Kernel::request_exit()
{
    if (!exit_callback_)
    {
        exit_requested_ = true;
        return;
    }
    auto &callback = callbacks_.at(*exit_callback_);
    if (callback.notifications == std::numeric_limits<std::uint32_t>::max())
    {
        throw std::overflow_error("Callback notification count overflow");
    }
    ++callback.notifications;
    exit_requested_ = false;
}

bool Kernel::enter_interrupt_callback(GuestAddress entry, std::uint32_t argument, GuestAddress common)
{
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        throw std::logic_error("Interrupt callback needs a ready thread");
    }
    if (!interrupts_enabled_ || thread.callback)
    {
        return false;
    }
    begin_callback(thread, entry, {argument, common.value_of(), 0}, std::nullopt);
    return true;
}

void Kernel::advance_time(std::uint64_t microseconds)
{
    if (microseconds > std::numeric_limits<std::uint64_t>::max() - system_time_)
    {
        throw std::overflow_error("Guest clock overflow");
    }
    // LCD vblank is 60000/1001 Hz (PSPSDK pspdisplay.h). Split the
    // calculation to keep it within uint64_t even at the clock's limit.
    const auto vblanks = [](std::uint64_t time)
    { return (time / 1'001'000) * 60 + (time % 1'001'000) * 60 / 1'001'000; };
    const auto next_time = system_time_ + microseconds;
    if (vblanks(next_time) != vblanks(system_time_))
    {
        interrupt_pending_ = true;
    }
    system_time_ = next_time;
}

std::uint64_t Kernel::system_time() const
{
    return system_time_;
}

std::uint64_t Kernel::next_vblank_time() const
{
    // Split at the exact 60-frame period to avoid drift and intermediate overflow.
    const auto period = system_time_ / 1'001'000;
    const auto edge = (system_time_ % 1'001'000) * 60 / 1'001'000 + 1;
    const auto offset = (edge * 1'001'000 + 59) / 60;
    const auto start = period * 1'001'000;
    if (offset > std::numeric_limits<std::uint64_t>::max() - start)
    {
        throw std::overflow_error("Next vblank exceeds guest clock range");
    }
    return start + offset;
}

bool Kernel::deliver_pending_interrupt()
{
    if (!interrupts_enabled_ || !interrupt_pending_)
    {
        return false;
    }
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started)
    {
        return false;
    }
    // Kernel interrupt entry/return is handled on the host. No guest handler
    // executes yet, so registers and both PCs already hold the resumed state.
    thread.state.load_linked = false;
    interrupt_pending_ = false;
    return true;
}

std::uint32_t Kernel::suspend_interrupts()
{
    const auto previous = static_cast<std::uint32_t>(interrupts_enabled_);
    interrupts_enabled_ = false;
    return previous;
}

void Kernel::resume_interrupts(std::uint32_t flags)
{
    interrupts_enabled_ = (flags & 1U) != 0;
}

bool Kernel::interrupts_enabled() const
{
    return interrupts_enabled_;
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
    if (thread.lifecycle == Lifecycle::Waiting)
    {
        status = ThreadStatus::Waiting;
        // PSP sleep/delay wait types; controller waits use an event flag approximation.
        switch (thread.wait)
        {
        case Wait::SleepCallback:
            info.wait_type = 1;
            break;
        case Wait::Delay:
            info.wait_type = 2;
            break;
        case Wait::Vblank:
            info.wait_type = 12;
            break;
        default:
            info.wait_type = 4;
            break;
        }
    }
    info.status = static_cast<std::uint32_t>(status);
    info.entry = creation.entry.value_of();
    info.stack = thread.stack.value_of();
    info.stack_size = creation.stack_size;
    info.global_pointer = global_pointer_.value_of();
    info.initial_priority = creation.priority;
    info.current_priority = creation.priority;
    info.exit_status = thread.lifecycle == Lifecycle::Finished ? thread.state.registers[2] : 0;
    // Scheduling counters remain zero in this cooperative runtime.
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

std::uint32_t Kernel::create_event_flag(const EventFlagCreation &creation)
{
    if ((creation.attributes & ~0x200U) != 0 || creation.options.value_of() != 0)
    {
        throw std::runtime_error("Unsupported event flag creation parameters");
    }
    memory_.read_c_string(creation.name, 32);
    const auto id = next_id_++;
    event_flags_.emplace(id, creation.bits);
    return id;
}

void Kernel::delete_event_flag(std::uint32_t id)
{
    if (event_flags_.erase(id) == 0)
    {
        throw std::runtime_error("Invalid event flag ID");
    }
}

std::uint32_t Kernel::allocate_partition(PartitionAllocation allocation)
{
    if (allocation.partition != 2 || allocation.type > 1)
    {
        throw std::runtime_error("Unsupported partition allocation");
    }
    const auto direction = allocation.type == 1 ? AllocationDirection::High : AllocationDirection::Low;
    const auto range = allocate_memory(allocation.size, direction);
    const auto id = next_id_++;
    blocks_.emplace(id, range);
    return id;
}

void Kernel::free_partition(std::uint32_t id)
{
    const auto range = blocks_.at(id);
    auto position =
        std::lower_bound(free_ranges_.begin(), free_ranges_.end(), range.begin,
                         [](const MemoryRange &free, std::uint64_t address) { return free.begin < address; });
    position = free_ranges_.insert(position, range);
    if (position != free_ranges_.begin() && std::prev(position)->end == position->begin)
    {
        std::prev(position)->end = position->end;
        position = std::prev(free_ranges_.erase(position));
    }
    if (std::next(position) != free_ranges_.end() && position->end == std::next(position)->begin)
    {
        position->end = std::next(position)->end;
        free_ranges_.erase(std::next(position));
    }
    blocks_.erase(id);
}

std::uint32_t Kernel::free_memory_size() const
{
    std::uint64_t total = 0;
    for (const auto &range : free_ranges_)
    {
        total += range.end - range.begin;
    }
    return static_cast<std::uint32_t>(total);
}

std::uint32_t Kernel::largest_free_memory_size() const
{
    std::uint64_t largest = 0;
    for (const auto &range : free_ranges_)
    {
        largest = std::max(largest, range.end - range.begin);
    }
    return static_cast<std::uint32_t>(largest);
}

GuestAddress Kernel::block_address(std::uint32_t id) const
{
    return GuestAddress{static_cast<std::uint32_t>(blocks_.at(id).begin)};
}

Kernel::MemoryRange Kernel::allocate_memory(std::uint32_t size, AllocationDirection direction)
{
    const auto aligned_size = (static_cast<std::uint64_t>(size) + 255) & ~std::uint64_t{255};
    if (size == 0)
    {
        throw std::runtime_error("Guest allocation size must be positive");
    }
    for (std::size_t offset = 0; offset < free_ranges_.size(); ++offset)
    {
        const auto index = direction == AllocationDirection::Low ? offset : free_ranges_.size() - 1 - offset;
        auto &range = free_ranges_[index];
        if (aligned_size > range.end - range.begin)
        {
            continue;
        }
        MemoryRange allocation{};
        if (direction == AllocationDirection::Low)
        {
            allocation = {range.begin, range.begin + aligned_size};
            range.begin = allocation.end;
        }
        else
        {
            allocation = {range.end - aligned_size, range.end};
            range.end = allocation.begin;
        }
        if (range.begin == range.end)
        {
            free_ranges_.erase(free_ranges_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        return allocation;
    }
    throw std::runtime_error("Guest memory exhausted");
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

void Kernel::wake_delayed_threads()
{
    while (!delayed_.empty() && delayed_.begin()->first <= system_time_)
    {
        const auto id = delayed_.begin()->second;
        auto &thread = threads_.at(id);
        thread.state.registers[2] = 0; // sceKernelDelayThread completed successfully.
        thread.lifecycle = Lifecycle::Started;
        thread.wait = Wait::None;
        ready_.push_back(id);
        delayed_.erase(delayed_.begin());
    }
}

void Kernel::wake_callbacks()
{
    for (auto &[id, callback] : callbacks_)
    {
        auto &thread = threads_.at(callback.owner);
        if (callback.notifications == 0 || thread.lifecycle != Lifecycle::Waiting ||
            thread.wait != Wait::SleepCallback || thread.callback)
        {
            continue;
        }
        begin_callback(thread, callback.entry, {callback.notifications, 0, callback.common.value_of()}, id);
        callback.notifications = 0;
        ready_.push_back(callback.owner);
    }
}

void Kernel::begin_callback(Thread &thread, GuestAddress entry, std::array<std::uint32_t, 3> arguments,
                            std::optional<std::uint32_t> id)
{
    if (entry.value_of() == 0 || (entry.value_of() & 3U) != 0)
    {
        throw std::runtime_error("Callback entry must be non-null and aligned");
    }
    memory_.validate_range(entry, 4);
    const auto stack_pointer = thread.state.registers[29];
    if (stack_pointer < thread.stack.value_of() + 16 ||
        static_cast<std::uint64_t>(stack_pointer) >
            static_cast<std::uint64_t>(thread.stack.value_of()) + thread.creation.stack_size)
    {
        throw std::runtime_error("Insufficient thread stack for callback entry");
    }
    thread.callback = CallbackContext{thread.state, id};
    thread.state.program_counter = entry;
    thread.state.next_program_counter = GuestAddress{entry.value_of() + 4};
    std::ranges::copy(arguments, thread.state.registers.begin() + 4);
    thread.state.registers[29] = (stack_pointer - 16) & ~15U;
    thread.state.registers[31] = return_address_.value_of() + 4;
    thread.state.load_linked = false;
    thread.lifecycle = Lifecycle::Started;
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
