#include "runtime/kernel.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace psp::detail
{

namespace
{

// First integer microsecond at or after the LCD edge following time, if the clock can represent it.
std::optional<std::uint64_t> vblank_after(std::uint64_t time)
{
    // One frame lasts cycle / frames microseconds, so `frames` edges span exactly `cycle`
    // microseconds. Splitting at whole cycles avoids drift and intermediate overflow.
    using FrameMicroseconds = std::ratio_divide<LcdFrames::period, std::micro>;
    constexpr std::uint64_t cycle = FrameMicroseconds::num;
    constexpr std::uint64_t frames = FrameMicroseconds::den;
    const auto period = time / cycle;
    const auto edge = (time % cycle) * frames / cycle + 1;
    const auto offset = (edge * cycle + frames - 1) / frames;
    const auto start = period * cycle;
    if (offset > std::numeric_limits<std::uint64_t>::max() - start)
    {
        return std::nullopt;
    }
    return start + offset;
}

// Delays, vblank waits, and vblank controller samples end as guest time passes.
// Callback notifications need host input; GE completion needs GE work.
bool guest_time_ends_wait(Kernel::Wait reason)
{
    switch (reason)
    {
    case Kernel::Wait::Delay:
    case Kernel::Wait::Vblank:
    case Kernel::Wait::Controller:
        return true;
    case Kernel::Wait::None:
    case Kernel::Wait::SleepCallback:
    case Kernel::Wait::Ge:
        return false;
    }
    throw std::logic_error("Unknown thread wait reason");
}

} // namespace

Kernel::Kernel(Memory &memory, AddressRange allocatable, GuestAddress global_pointer)
    : memory_(memory), arena_(allocatable.begin, allocatable.end), global_pointer_(global_pointer),
      return_address_(GuestAddress{static_cast<std::uint32_t>(arena_.allocate(256, AllocationDirection::Low).begin)}),
      next_vblank_(vblank_after(0))
{
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
        GuestAddress{static_cast<std::uint32_t>(arena_.allocate(creation.stack_size, AllocationDirection::High).begin)};
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

ThreadSelection Kernel::select_next_thread()
{
    if (exit_code_)
    {
        return ThreadSelection::Finished;
    }
    auto &thread = threads_.at(current_thread_);
    complete_return(thread);
    // A GE/controller event can wake the selected idle thread between selections.
    // It is already running once awakened; consume its ready-queue entry here.
    if (thread.lifecycle == Lifecycle::Started)
    {
        std::erase(ready_, current_thread_);
    }
    const bool select_ready = thread.lifecycle != Lifecycle::Started;
    wake_delayed_threads();
    wake_callbacks();
    if (thread.callback && !thread.callback->id)
    {
        return ThreadSelection::Ready;
    }
    if (select_ready && ready_.empty())
    {
        if (std::ranges::any_of(threads_, [](const auto &item) { return item.second.lifecycle == Lifecycle::Waiting; }))
        {
            return ThreadSelection::Idle;
        }
        exit_code_ = std::bit_cast<std::int32_t>(thread.state.registers[2]);
        return ThreadSelection::Finished;
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
    delay_until(system_time_ + microseconds, Wait::Delay);
}

void Kernel::wait(Wait reason)
{
    switch (reason)
    {
    case Wait::Controller:
    case Wait::Ge:
        block_current_thread(reason);
        return;
    case Wait::None:
    case Wait::SleepCallback:
    case Wait::Delay:
    case Wait::Vblank:
        break;
    }
    throw std::invalid_argument("This wait reason has its own kernel service");
}

void Kernel::wake(std::uint32_t id, Wait reason, std::uint32_t result)
{
    auto &thread = threads_.at(id);
    if (thread.callback && !thread.callback->id)
    {
        auto &interrupted = *thread.callback;
        if (interrupted.lifecycle != Lifecycle::Waiting || interrupted.wait != reason)
        {
            throw std::logic_error("Interrupted thread is not waiting for this event");
        }
        interrupted.state.registers[2] = result;
        interrupted.lifecycle = Lifecycle::Started;
        interrupted.wait = Wait::None;
        return;
    }
    if (thread.lifecycle != Lifecycle::Waiting || thread.wait != reason)
    {
        throw std::logic_error("Thread is not waiting for this event");
    }
    thread.state.registers[2] = result;
    thread.lifecycle = Lifecycle::Started;
    thread.wait = Wait::None;
    ready_.push_back(id);
}

void Kernel::wait_vblank()
{
    delay_until(next_vblank_time(), Wait::Vblank);
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

bool Kernel::interrupt_callback_ready() const
{
    return interrupts_enabled_ &&
           !std::ranges::any_of(threads_, [](const auto &item) { return static_cast<bool>(item.second.callback); }) &&
           std::ranges::any_of(
               threads_, [](const auto &item)
               { return item.second.lifecycle == Lifecycle::Started || item.second.lifecycle == Lifecycle::Waiting; });
}

bool Kernel::untimed_waits_only() const
{
    const auto untimed = [](const auto &item)
    {
        const auto &thread = item.second;
        return thread.lifecycle == Lifecycle::Created || thread.lifecycle == Lifecycle::Finished ||
               (thread.lifecycle == Lifecycle::Waiting && !thread.callback && !guest_time_ends_wait(thread.wait));
    };
    return !exit_code_ && std::ranges::all_of(threads_, untimed) &&
           std::ranges::none_of(callbacks_, [](const auto &item) { return item.second.notifications != 0; });
}

bool Kernel::enter_interrupt_callback(GuestAddress entry, std::uint32_t argument, GuestAddress common)
{
    if (!interrupt_callback_ready())
    {
        return false;
    }
    // The last running thread may have exited while other threads still wait.
    // Use the highest-priority waiting thread as the idle interrupt context.
    if (threads_.at(current_thread_).lifecycle == Lifecycle::Finished)
    {
        std::optional<std::uint32_t> selected;
        for (const auto &[id, candidate] : threads_)
        {
            if (candidate.lifecycle == Lifecycle::Waiting &&
                (!selected || candidate.creation.priority < threads_.at(*selected).creation.priority))
            {
                selected = id;
            }
        }
        if (!selected)
        {
            return false;
        }
        current_thread_ = *selected;
    }
    auto &thread = threads_.at(current_thread_);
    begin_callback(thread, entry, {argument, common.value_of(), 0}, std::nullopt);
    interrupts_enabled_ = false;
    return true;
}

bool Kernel::advance_time(std::uint64_t microseconds)
{
    if (microseconds > std::numeric_limits<std::uint64_t>::max() - system_time_)
    {
        throw std::overflow_error("Guest clock overflow");
    }
    system_time_ += microseconds;
    if (!next_vblank_ || system_time_ < *next_vblank_)
    {
        return false;
    }
    interrupt_pending_ = true;
    next_vblank_ = vblank_after(system_time_);
    return true;
}

std::uint64_t Kernel::system_time() const
{
    return system_time_;
}

std::uint64_t Kernel::next_vblank_time() const
{
    if (!next_vblank_)
    {
        throw std::overflow_error("Next vblank exceeds guest clock range");
    }
    return *next_vblank_;
}

std::optional<std::uint64_t> Kernel::next_wakeup_time() const
{
    if (delayed_.empty())
    {
        return std::nullopt;
    }
    return delayed_.begin()->first;
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
    const auto &thread = threads_.at(current_thread_);
    if (thread.callback && !thread.callback->id)
    {
        throw std::runtime_error("Exiting a thread from a GE interrupt callback is unsupported");
    }
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
        // PSP sleep/delay wait types; controller and GE waits use an event flag approximation.
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
        case Wait::Controller:
        case Wait::Ge:
            info.wait_type = 4;
            break;
        case Wait::None:
            throw std::logic_error("Waiting thread has no wait reason");
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
    const auto range = arena_.allocate(allocation.size, direction);
    const auto id = next_id_++;
    blocks_.emplace(id, range);
    return id;
}

void Kernel::free_partition(std::uint32_t id)
{
    arena_.free(blocks_.at(id));
    blocks_.erase(id);
}

std::uint32_t Kernel::free_memory_size() const
{
    return static_cast<std::uint32_t>(arena_.free_size());
}

std::uint32_t Kernel::largest_free_memory_size() const
{
    return static_cast<std::uint32_t>(arena_.largest_free_size());
}

GuestAddress Kernel::block_address(std::uint32_t id) const
{
    return GuestAddress{static_cast<std::uint32_t>(blocks_.at(id).begin)};
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

void Kernel::complete_return(Thread &thread)
{
    if (thread.lifecycle != Lifecycle::Started)
    {
        return;
    }
    if (thread.callback && thread.state.program_counter == GuestAddress{return_address_.value_of() + 4})
    {
        const auto callback_id = thread.callback->id;
        const auto remove = thread.state.registers[2] != 0;
        thread.state = thread.callback->state;
        thread.state.load_linked = false;
        thread.lifecycle = thread.callback->lifecycle;
        thread.wait = thread.callback->wait;
        if (!callback_id)
        {
            interrupts_enabled_ = thread.callback->interrupts_enabled;
        }
        thread.callback.reset();
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
}

void Kernel::delay_until(std::uint64_t deadline, Wait reason)
{
    block_current_thread(reason);
    delayed_.emplace(deadline, current_thread_);
}

void Kernel::wake_delayed_threads()
{
    while (!delayed_.empty() && delayed_.begin()->first <= system_time_)
    {
        const auto id = delayed_.begin()->second;
        const auto &thread = threads_.at(id);
        const auto wait = thread.callback && !thread.callback->id ? thread.callback->wait : thread.wait;
        wake(id, wait, 0);
        delayed_.erase(delayed_.begin());
    }
}

void Kernel::wake_callbacks()
{
    for (auto &[id, callback] : callbacks_)
    {
        if (callback.notifications == 0)
        {
            continue;
        }
        auto &thread = threads_.at(callback.owner);
        if (thread.lifecycle != Lifecycle::Waiting || thread.wait != Wait::SleepCallback || thread.callback)
        {
            continue;
        }
        begin_callback(thread, callback.entry, {callback.notifications, 0, callback.common.value_of()}, id);
        callback.notifications = 0;
        ready_.push_back(callback.owner);
    }
}

void Kernel::block_current_thread(Wait reason)
{
    auto &thread = threads_.at(current_thread_);
    if (thread.lifecycle != Lifecycle::Started || (thread.callback && !thread.callback->id))
    {
        throw std::logic_error("Only a running thread outside a GE interrupt callback can wait");
    }
    thread.lifecycle = Lifecycle::Waiting;
    thread.wait = reason;
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
    thread.callback = CallbackContext{thread.state, id, thread.lifecycle, thread.wait, interrupts_enabled_};
    thread.state.program_counter = entry;
    thread.state.next_program_counter = GuestAddress{entry.value_of() + 4};
    std::ranges::copy(arguments, thread.state.registers.begin() + 4);
    thread.state.registers[29] = (stack_pointer - 16) & ~15U;
    thread.state.registers[31] = return_address_.value_of() + 4;
    thread.state.load_linked = false;
    thread.lifecycle = Lifecycle::Started;
    thread.wait = Wait::None;
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
