#include "runtime/threads.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace psp::detail
{

Threads::Threads(Memory &memory, GuestAllocator &allocator, std::uint32_t &next_id, GuestAddress global_pointer)
    : memory_(memory), allocator_(allocator), next_id_(next_id), global_pointer_(global_pointer),
      return_address_(allocator.allocate(256, false))
{
}

void Threads::initialize(GuestAddress entry, std::span<const std::string> arguments)
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
    current_thread_ = create({entry, stack_size, 0x20, "startup", 0x80000000});
    auto &initial = threads_.at(current_thread_);
    place_arguments(initial, bytes);
    initial.lifecycle = Lifecycle::Started;
}

std::uint32_t Threads::create(ThreadCreation creation)
{
    if (creation.stack_size < 512)
    {
        throw std::runtime_error("Thread stack must contain at least 512 bytes");
    }
    const auto stack = allocator_.allocate(creation.stack_size, true);
    CpuState state{.program_counter = creation.entry};
    state.registers[28] = global_pointer_.value_of();
    state.registers[31] = return_address_.value_of();
    const auto id = next_id_++;
    threads_.emplace(id, Thread{std::move(creation), state, stack});
    return id;
}

void Threads::start(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size)
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

bool Threads::select_next()
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

CpuState &Threads::current_state()
{
    return threads_.at(current_thread_).state;
}

std::uint32_t Threads::current_id() const
{
    return current_thread_;
}

std::uint32_t Threads::current_priority() const
{
    return threads_.at(current_thread_).creation.priority;
}

void Threads::exit_current()
{
    threads_.at(current_thread_).lifecycle = Lifecycle::Finished;
}

void Threads::exit_game()
{
    exit_code_ = 0;
}

int Threads::exit_code() const
{
    if (!exit_code_)
    {
        throw std::logic_error("Execution has not finished");
    }
    return *exit_code_;
}

GuestThreadInfo Threads::status(std::uint32_t id) const
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

void Threads::place_arguments(Thread &thread, PayloadSpan arguments)
{
    // Arguments sit at the top of the stack, above an aligned initial call frame.
    const auto address =
        thread.stack.value_of() + thread.creation.stack_size - static_cast<std::uint32_t>(arguments.size());
    memory_.write_bytes(GuestAddress{address}, arguments);
    thread.state.registers[4] = static_cast<std::uint32_t>(arguments.size());
    thread.state.registers[5] = address;
    thread.state.registers[29] = (address & ~15U) - 64;
}

} // namespace psp::detail
