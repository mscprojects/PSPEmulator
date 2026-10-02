#include "runtime/runtime.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace psp::detail
{

namespace
{

std::string hexadecimal(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

// Encode PSP startup arguments as consecutive NUL-terminated strings. The returned
// bytes own their storage; embedded NULs are rejected because they split arguments.
Payload encode_arguments(std::span<const std::string> arguments)
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
    return bytes;
}

// Copy argument bytes to the top of an allocated stack and initialize a0 (size),
// a1 (address), and sp with alignment and call-frame space. The caller must ensure
// arguments fit in the stack with at least 256 bytes left for the initial frame.
void place_arguments(Memory &memory, CpuState &state, GuestAddress stack, std::uint32_t stack_size,
                     PayloadSpan arguments)
{
    const auto address = stack.value_of() + stack_size - static_cast<std::uint32_t>(arguments.size());
    memory.write_bytes(GuestAddress{address}, arguments);
    state.registers[4] = static_cast<std::uint32_t>(arguments.size());
    state.registers[5] = address;
    state.registers[29] = (address & ~15U) - 64;
}

// Read one of the eight Allegrex integer service arguments (a0-a3 and t0-t3).
// Reject indices that would require unsupported stack argument decoding.
std::uint32_t service_argument(const CpuState &state, std::size_t index)
{
    if (index >= 8)
    {
        throw std::logic_error("Unsupported service argument index");
    }
    return state.registers[4 + index];
}

// Copy a NUL-terminated guest string; reject address overflow or no terminator
// within 4096 bytes. Memory enforces the mapped region bounds.
std::string read_guest_string(const Memory &memory, GuestAddress address)
{
    std::string result;
    for (std::uint32_t index = 0; index < 4096; ++index)
    {
        if (static_cast<std::uint64_t>(address.value_of()) + index > 0xFFFFFFFFU)
        {
            throw std::runtime_error("Service string exceeds address space");
        }
        const auto byte = memory.read_u8(GuestAddress{address.value_of() + index});
        if (byte == 0)
        {
            return result;
        }
        result.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("Unterminated service string");
}

// Copy a guest buffer, rejecting lengths beyond memory_size and address overflow.
// An empty buffer is valid without dereferencing its address.
std::string read_guest_bytes(const Memory &memory, std::size_t memory_size, GuestAddress address, std::uint32_t size)
{
    if (size > memory_size || static_cast<std::uint64_t>(address.value_of()) + size > (std::uint64_t{1} << 32))
    {
        throw std::runtime_error("Invalid service buffer range");
    }
    std::string result;
    for (std::uint32_t index = 0; index < size; ++index)
    {
        result.push_back(static_cast<char>(memory.read_u8(GuestAddress{address.value_of() + index})));
    }
    return result;
}

} // namespace

Runtime::Runtime(const ParsedPrx &prx, const ExecutionOptions &options)
    : loaded_(prepare_prx(prx, options.load_address, options.memory_size)), cpu_(loaded_.memory), options_(options),
      heap_(options.load_address.value_of()),
      stack_top_((static_cast<std::uint64_t>(options.load_address.value_of()) + options.memory_size) &
                 ~std::uint64_t{255})
{
    if (options.max_instructions == 0)
    {
        throw std::invalid_argument("Instruction budget must be positive");
    }
    for (const auto &segment : prx.segments)
    {
        if (segment.type == 1)
        {
            heap_ = std::max(heap_, static_cast<std::uint64_t>(options.load_address.value_of()) +
                                        segment.virtual_address + segment.memory_size);
        }
    }
    heap_ = (heap_ + 255) & ~std::uint64_t{255};
    return_address_ = allocate(256, false);
    std::uint32_t code = 1;
    for (const auto &library : loaded_.imports)
    {
        for (const auto &function : library.functions)
        {
            if (code > 0xFFFFF)
            {
                throw std::invalid_argument("Too many PRX imports");
            }
            imports_.emplace(code, ImportBinding{library.name, function.nid});
            // Preserve JR's delay slot: the CPU reports the service after committing
            // the return target, and the runtime supplies its result before continuing.
            loaded_.memory.write_u32(function.stub_address, 0x03E00008);
            loaded_.memory.write_u32(GuestAddress{function.stub_address.value_of() + 4}, (code << 6) | 0xC);
            ++code;
        }
    }
    current_thread_ = create_thread(loaded_.entry_point, 0x10000, 0x20);
    auto &initial = threads_.at(current_thread_);
    initial.started = true;
    const auto arguments = encode_arguments(options.arguments);
    if (arguments.size() > 0x10000 - 256)
    {
        throw std::invalid_argument("Program arguments exceed startup stack capacity");
    }
    place_arguments(loaded_.memory, initial.state, GuestAddress{initial.stack}, initial.stack_size, arguments);
}

ExecutionResult Runtime::run()
{
    while (!exit_code_)
    {
        auto &thread = threads_.at(current_thread_);
        auto &state = thread.state;
        if (state.program_counter.value_of() == return_address_ || thread.finished)
        {
            thread.finished = true;
            if (ready_.empty())
            {
                exit_code_ = std::bit_cast<std::int32_t>(state.registers[2]);
                break;
            }
            current_thread_ = ready_.front();
            ready_.pop_front();
            continue;
        }
        const auto pc = state.program_counter;
        try
        {
            if (instructions_ == options_.max_instructions)
            {
                throw std::runtime_error("Instruction budget exhausted");
            }
            const auto syscall = cpu_.step(state);
            ++instructions_;
            if (syscall)
            {
                const auto binding = imports_.find(*syscall);
                if (binding == imports_.end())
                {
                    throw std::runtime_error("Unbound syscall " + hexadecimal(*syscall));
                }
                state.registers[2] = service(state, binding->second);
            }
        }
        catch (const std::exception &error)
        {
            std::string context = "PRX execution at " + hexadecimal(pc.value_of());
            try
            {
                context += " (instruction " + hexadecimal(loaded_.memory.read_u32(pc)) + ")";
            }
            catch (const std::out_of_range &)
            {
                // A fetch outside mapped memory has no instruction word to report.
                context += " (instruction unavailable)";
            }
            throw std::runtime_error(context + ": " + error.what());
        }
    }
    return {std::move(output_), *exit_code_, instructions_};
}

std::uint32_t Runtime::allocate(std::uint32_t size, bool high)
{
    // A monotonic arena suffices for this milestone. Partition frees are unsupported
    // and fail at dispatch; no successful free silently leaks a live allocation.
    const auto aligned_size = (static_cast<std::uint64_t>(size) + 255) & ~std::uint64_t{255};
    if (size == 0 || heap_ > stack_top_ || aligned_size > stack_top_ - heap_)
    {
        throw std::runtime_error("Guest memory exhausted");
    }
    if (high)
    {
        stack_top_ -= aligned_size;
        return static_cast<std::uint32_t>(stack_top_);
    }
    const auto address = static_cast<std::uint32_t>(heap_);
    heap_ += aligned_size;
    return address;
}

std::uint32_t Runtime::create_thread(GuestAddress entry, std::uint32_t stack_size, std::uint32_t priority)
{
    if (stack_size < 512)
    {
        throw std::runtime_error("Thread stack must contain at least 512 bytes");
    }
    const auto stack = allocate(stack_size, true);
    CpuState state{.program_counter = entry};
    state.registers[28] = loaded_.module.global_pointer.value_of();
    state.registers[29] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(stack) + stack_size - 64);
    state.registers[31] = return_address_;
    const auto id = next_id_++;
    threads_.emplace(id, Thread{state, stack, stack_size, priority});
    return id;
}

std::uint32_t Runtime::service(CpuState &state, const ImportBinding &binding)
{
    const auto arg = [&](std::size_t index) { return service_argument(state, index); };
    if (binding.library == "ThreadManForUser")
    {
        switch (binding.nid)
        {
        case 0x446D8DE6: // sceKernelCreateThread
            return create_thread(GuestAddress{arg(1)}, arg(3), arg(2));
        case 0xF475845D: // sceKernelStartThread
        {
            auto &thread = threads_.at(arg(0));
            if (thread.started)
            {
                throw std::runtime_error("Thread already started");
            }
            const auto bytes = read_guest_bytes(loaded_.memory, options_.memory_size, GuestAddress{arg(2)}, arg(1));
            if (bytes.size() > thread.stack_size - 256)
            {
                throw std::runtime_error("Thread arguments exceed stack capacity");
            }
            place_arguments(loaded_.memory, thread.state, GuestAddress{thread.stack}, thread.stack_size,
                            PayloadSpan{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
            thread.started = true;
            ready_.push_back(arg(0));
            return 0;
        }
        case 0xAA73C935: // sceKernelExitThread
            threads_.at(current_thread_).finished = true;
            return arg(0);
        case 0x94AA61EE: // sceKernelGetThreadCurrentPriority
            return threads_.at(current_thread_).priority;
        case 0x82BC5777: // sceKernelGetSystemTimeWide: deterministic logical time.
            state.registers[3] = static_cast<std::uint32_t>(instructions_ >> 32);
            return static_cast<std::uint32_t>(instructions_);
        default:
            break;
        }
    }
    if (binding.library == "SysMemUserForUser")
    {
        switch (binding.nid)
        {
        case 0xA291F107: // sceKernelMaxFreeMemSize
        case 0xF919F628: // sceKernelTotalFreeMemSize
            return static_cast<std::uint32_t>(stack_top_ - heap_);
        case 0x237DBD4F: // sceKernelAllocPartitionMemory
        {
            if (arg(0) != 2 || arg(2) > 1)
            {
                throw std::runtime_error("Unsupported partition allocation");
            }
            const auto id = next_id_++;
            blocks_.emplace(id, allocate(arg(3), arg(2) == 1));
            return id;
        }
        case 0x9D9A5BA1: // sceKernelGetBlockHeadAddr
            return blocks_.at(arg(0));
        default:
            break;
        }
    }
    if (binding.library == "StdioForUser")
    {
        switch (binding.nid)
        {
        case 0x172D316E:
            return 0; // stdin
        case 0xA6BAB2E9:
            return 1; // stdout
        case 0xF78BA90A:
            return 2; // stderr
        default:
            break;
        }
    }
    if (binding.library == "IoFileMgrForUser" && binding.nid == 0x42EC03AC) // sceIoWrite
    {
        if (arg(0) != 1 && arg(0) != 2)
        {
            throw std::runtime_error("Only stdout and stderr writes are supported");
        }
        output_ += read_guest_bytes(loaded_.memory, options_.memory_size, GuestAddress{arg(1)}, arg(2));
        return arg(2);
    }
    if (binding.library == "IoFileMgrForUser" && binding.nid == 0xB29DDF9C) // sceIoDopen
    {
        // No guest filesystem is mounted in the headless runtime.
        return 0x80010002; // ENOENT
    }
    if (binding.library == "IoFileMgrForUser" && binding.nid == 0x54F5FB11) // sceIoDevctl
    {
        const auto device = read_guest_string(loaded_.memory, GuestAddress{arg(0)});
        if (device != "emulator:" && device != "kemulator:")
        {
            throw std::runtime_error("Unsupported devctl device " + device);
        }
        switch (arg(1))
        {
        case 1: // Headless runtime has no display.
            if (arg(5) < 4)
            {
                throw std::runtime_error("Display query buffer is too small");
            }
            loaded_.memory.write_u32(GuestAddress{arg(4)}, 0);
            return 0;
        case 2: // Capture the bytes produced by guest libc, not host formatting.
            output_ += read_guest_bytes(loaded_.memory, options_.memory_size, GuestAddress{arg(2)}, arg(3));
            return 0;
        case 3: // Emulator probe.
            return 0;
        default:
            throw std::runtime_error("Unsupported emulator devctl command");
        }
    }
    if (binding.library == "LoadExecForUser" && binding.nid == 0x05572A5F) // sceKernelExitGame
    {
        exit_code_ = 0;
        return 0;
    }
    throw std::runtime_error("Unsupported import " + binding.library + ":" + hexadecimal(binding.nid));
}

} // namespace psp::detail
