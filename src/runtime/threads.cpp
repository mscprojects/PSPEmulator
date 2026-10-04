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
    if (bytes.size() > 0x10000 - 256)
    {
        throw std::invalid_argument("Program arguments exceed startup stack capacity");
    }
    current_thread_ = create({entry, 0x10000, 0x20, "startup", 0x80000000});
    auto &initial = threads_.at(current_thread_);
    place_arguments(initial, bytes);
    initial.started = true;
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
    state.registers[29] =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(stack.value_of()) + creation.stack_size - 64);
    state.registers[31] = return_address_.value_of();
    const auto id = next_id_++;
    threads_.emplace(id, Thread{state, stack, creation.stack_size, creation.priority, false, false, creation.entry,
                                std::move(creation.name), creation.attributes});
    return id;
}

void Threads::start(std::uint32_t id, GuestAddress arguments, std::uint32_t argument_size)
{
    auto &thread = threads_.at(id);
    if (thread.started)
    {
        throw std::runtime_error("Thread already started");
    }
    const auto bytes = memory_.read_bytes(arguments, argument_size);
    if (bytes.size() > thread.stack_size - 256)
    {
        throw std::runtime_error("Thread arguments exceed stack capacity");
    }
    place_arguments(thread, bytes);
    thread.started = true;
    ready_.push_back(id);
}

bool Threads::select_next()
{
    if (exit_code_)
    {
        return false;
    }
    auto &thread = threads_.at(current_thread_);
    if (thread.state.program_counter == return_address_ || thread.finished)
    {
        thread.finished = true;
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
    return threads_.at(current_thread_).priority;
}

void Threads::exit_current()
{
    threads_.at(current_thread_).finished = true;
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
    GuestThreadInfo info;
    const auto name_size = static_cast<std::ptrdiff_t>(std::min(thread.name.size(), info.name.size() - 1));
    std::ranges::copy_n(thread.name.begin(), name_size, info.name.begin());
    info.attributes = thread.attributes;
    const auto status = [&]
    {
        if (thread.finished || !thread.started)
        {
            return ThreadStatus::Stopped;
        }
        return id == current_thread_ ? ThreadStatus::Running : ThreadStatus::Ready;
    }();
    info.status = static_cast<std::uint32_t>(status);
    info.entry = thread.entry.value_of();
    info.stack = thread.stack.value_of();
    info.stack_size = thread.stack_size;
    info.global_pointer = global_pointer_.value_of();
    info.initial_priority = thread.priority;
    info.current_priority = thread.priority;
    info.exit_status = thread.finished ? thread.state.registers[2] : 0;
    // Wait and scheduling counters remain zero in this cooperative runtime.
    return info;
}

void Threads::place_arguments(Thread &thread, PayloadSpan arguments)
{
    // Arguments sit at the top of the stack, above an aligned initial call frame.
    const auto address = thread.stack.value_of() + thread.stack_size - static_cast<std::uint32_t>(arguments.size());
    memory_.write_bytes(GuestAddress{address}, arguments);
    thread.state.registers[4] = static_cast<std::uint32_t>(arguments.size());
    thread.state.registers[5] = address;
    thread.state.registers[29] = (address & ~15U) - 64;
}

} // namespace psp::detail
