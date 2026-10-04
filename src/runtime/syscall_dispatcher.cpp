#include "runtime/syscall_dispatcher.hpp"

#include <fmt/format.h>

#include <stdexcept>

namespace psp::detail
{

SyscallDispatcher::SyscallDispatcher(Memory &memory, Kernel &kernel, GuestAllocator &allocator, GuestIo &io,
                                     std::span<const PrxImportLibrary> imports)
    : memory_(memory), kernel_(kernel), allocator_(allocator), io_(io)
{
    std::uint32_t code = 1;
    for (const auto &library : imports)
    {
        for (const auto &function : library.functions)
        {
            if (code > 0xFFFFF)
            {
                throw std::invalid_argument("Too many PRX imports");
            }
            imports_.emplace(code, ImportBinding{library.name, function.nid});
            // JR schedules the return; its SYSCALL delay slot hands control to
            // the runtime after the CPU commits the return target.
            memory_.write_u32(function.stub_address, 0x03E00008);
            memory_.write_u32(GuestAddress{function.stub_address.value_of() + 4}, (code << 6) | 0xC);
            ++code;
        }
    }
}

void SyscallDispatcher::handle(const Syscall &syscall, CpuState &state, std::uint64_t instructions)
{
    const auto binding = imports_.find(syscall.code);
    if (binding == imports_.end())
    {
        throw std::runtime_error(fmt::format("Unbound syscall 0x{:x}", syscall.code));
    }
    state.registers[2] = dispatch(state, binding->second, instructions);
}

std::uint32_t SyscallDispatcher::dispatch(CpuState &state, const ImportBinding &binding, std::uint64_t instructions)
{
    // Allegrex integer service arguments occupy a0-a3 and t0-t3 (registers 4-11).
    const auto arg = [&](std::size_t index) { return state.registers.at(4 + index); };
    auto &threads = kernel_.threads;
    auto &synchronization = kernel_.synchronization;
    if (binding.library == "ThreadManForUser")
    {
        switch (binding.nid)
        {
        case 0x446D8DE6: // sceKernelCreateThread
            return threads.create(
                {GuestAddress{arg(1)}, arg(3), arg(2), memory_.read_c_string(GuestAddress{arg(0)}, 32), arg(4)});
        case 0xF475845D: // sceKernelStartThread
            threads.start(arg(0), GuestAddress{arg(2)}, arg(1));
            return 0;
        case 0xAA73C935: // sceKernelExitThread
            threads.exit_current();
            return arg(0);
        case 0x293B45B8: // sceKernelGetThreadId
            return threads.current_id();
        case 0x94AA61EE: // sceKernelGetThreadCurrentPriority
            return threads.current_priority();
        case 0x17C1684E: // sceKernelReferThreadStatus(a0: thread ID, a1: output pointer)
        {
            const auto info = threads.status(arg(0));
            const auto address = GuestAddress{arg(1)};
            if (memory_.read_u32(address) != sizeof(GuestThreadInfo))
            {
                throw std::runtime_error("Unsupported thread status structure size");
            }
            write_thread_info(memory_, address, info);
            return 0;
        }
        case 0x82BC5777: // sceKernelGetSystemTimeWide: deterministic logical time.
            state.registers[3] = static_cast<std::uint32_t>(instructions >> 32);
            return static_cast<std::uint32_t>(instructions);
        case 0x19CFF145: // sceKernelCreateLwMutex
            synchronization.create_mutex(
                {GuestAddress{arg(0)}, GuestAddress{arg(1)}, arg(2), arg(3), GuestAddress{arg(4)}},
                threads.current_id());
            return 0;
        case 0x60107536: // sceKernelDeleteLwMutex
            synchronization.delete_mutex(GuestAddress{arg(0)});
            return 0;
        case 0xD6DA4BA1: // sceKernelCreateSema
            return synchronization.create_semaphore(
                {GuestAddress{arg(0)}, arg(1), arg(2), arg(3), GuestAddress{arg(4)}});
        case 0x28B6489C: // sceKernelDeleteSema
            synchronization.delete_semaphore(arg(0));
            return 0;
        case 0x4E3A1105: // sceKernelWaitSema
            synchronization.wait_semaphore(arg(0), arg(1), GuestAddress{arg(2)});
            return 0;
        case 0x3F53E640: // sceKernelSignalSema
            synchronization.signal_semaphore(arg(0), arg(1));
            return 0;
        default:
            break;
        }
    }
    if (binding.library == "Kernel_Library")
    {
        switch (binding.nid)
        {
        case 0xBEA46419: // sceKernelLockLwMutex
            synchronization.lock_mutex(GuestAddress{arg(0)}, arg(1), GuestAddress{arg(2)}, threads.current_id());
            return 0;
        case 0x15B6446B: // sceKernelUnlockLwMutex
            synchronization.unlock_mutex(GuestAddress{arg(0)}, arg(1), threads.current_id());
            return 0;
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
            return allocator_.free_size();
        case 0x237DBD4F: // sceKernelAllocPartitionMemory
            return kernel_.allocate_partition({arg(0), arg(2), arg(3)});
        case 0x9D9A5BA1: // sceKernelGetBlockHeadAddr
            return allocator_.block_address(arg(0)).value_of();
        default:
            break;
        }
    }
    if (binding.library == "IoFileMgrForUser")
    {
        switch (binding.nid)
        {
        case 0x42EC03AC: // sceIoWrite
            return io_.write(arg(0), GuestAddress{arg(1)}, arg(2));
        case 0xB29DDF9C:       // sceIoDopen: no guest filesystem is mounted.
            return 0x80010002; // ENOENT
        case 0x54F5FB11:       // sceIoDevctl
            io_.device_control(
                {GuestAddress{arg(0)}, arg(1), GuestAddress{arg(2)}, arg(3), GuestAddress{arg(4)}, arg(5)});
            return 0;
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
    if (binding.library == "sceUtility" && binding.nid == 0xA5DA2406) // sceUtilityGetSystemParamInt
    {
        if (arg(0) != 6 && arg(0) != 7)
        {
            throw std::runtime_error("Unsupported integer system parameter");
        }
        // Deterministic UTC timezone with daylight saving disabled.
        memory_.write_u32(GuestAddress{arg(1)}, 0);
        return 0;
    }
    if (binding.library == "LoadExecForUser" && binding.nid == 0x05572A5F) // sceKernelExitGame
    {
        threads.exit_game();
        return 0;
    }
    throw std::runtime_error(fmt::format("Unsupported import {}:0x{:x}", binding.library, binding.nid));
}

} // namespace psp::detail
