#include "runtime/syscall_dispatcher.hpp"

#include <fmt/format.h>

#include <stdexcept>

namespace psp::detail
{

SyscallDispatcher::SyscallDispatcher(Memory &memory, Kernel &kernel, GuestIo &io, Display &display,
                                     Controller &controller, std::span<const PrxImportLibrary> imports)
    : memory_(memory), kernel_(kernel), io_(io), display_(display), controller_(controller)
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
            imports_.push_back({.library = library.name, .nid = function.nid});
            // JR schedules the return; its SYSCALL delay slot hands control to
            // the runtime after the CPU commits the return target.
            memory_.write_u32(function.stub_address, 0x03E00008);
            memory_.write_u32(GuestAddress{function.stub_address.value_of() + 4}, (code << 6) | 0xC);
            ++code;
        }
    }
}

void SyscallDispatcher::handle(const Syscall &syscall, CpuState &state)
{
    if (syscall.code == 0 || syscall.code > imports_.size())
    {
        throw std::runtime_error(fmt::format("Unbound syscall 0x{:x}", syscall.code));
    }
    if (const auto result = dispatch(state, imports_[syscall.code - 1]))
    {
        state.registers[2] = *result;
    }
}

std::optional<std::uint32_t> SyscallDispatcher::dispatch(CpuState &state, const ImportBinding &binding)
{
    // PSP MIPS32 EABI integer arguments occupy a0-a7 (registers 4-11).
    const auto arg = [&](std::size_t index) { return state.registers.at(4 + index); };
    if (binding.library == "sceGe_user" && binding.nid == 0xE47E40E4) // sceGeEdramGetAddr
    {
        return 0x04000000;
    }
    if (binding.library == "sceDisplay")
    {
        switch (binding.nid)
        {
        case 0x0E20F177: // sceDisplaySetMode
            display_.set_mode(arg(0), arg(1), arg(2));
            return 0;
        case 0x289D82FE: // sceDisplaySetFrameBuf
            display_.set_framebuffer({GuestAddress{arg(0)}, arg(1), arg(2)}, arg(3));
            return 0;
        default:
            break;
        }
    }
    if (binding.library == "ThreadManForUser")
    {
        switch (binding.nid)
        {
        case 0xE81CAF8F: // sceKernelCreateCallback
            memory_.read_c_string(GuestAddress{arg(0)}, 32);
            return kernel_.create_callback(GuestAddress{arg(1)}, GuestAddress{arg(2)});
        case 0x82826F70: // sceKernelSleepThreadCB
            kernel_.sleep_thread_callbacks();
            return std::nullopt;
        case 0x446D8DE6: // sceKernelCreateThread
            return kernel_.create_thread({.entry = GuestAddress{arg(1)},
                                          .stack_size = arg(3),
                                          .priority = arg(2),
                                          .name = memory_.read_c_string(GuestAddress{arg(0)}, 32),
                                          .attributes = arg(4)});
        case 0xF475845D: // sceKernelStartThread
            kernel_.start_thread(arg(0), GuestAddress{arg(2)}, arg(1));
            return 0;
        case 0xAA73C935: // sceKernelExitThread
            kernel_.exit_thread();
            return arg(0);
        case 0x293B45B8: // sceKernelGetThreadId
            return kernel_.current_thread_id();
        case 0x94AA61EE: // sceKernelGetThreadCurrentPriority
            return kernel_.current_thread_priority();
        case 0x17C1684E: // sceKernelReferThreadStatus(a0: thread ID, a1: output pointer)
        {
            const auto info = kernel_.thread_status(arg(0));
            const auto address = GuestAddress{arg(1)};
            if (memory_.read_u32(address) != sizeof(GuestThreadInfo))
            {
                throw std::runtime_error("Unsupported thread status structure size");
            }
            write_thread_info(memory_, address, info);
            return 0;
        }
        case 0x369ED59D: // sceKernelGetSystemTimeLow
            return static_cast<std::uint32_t>(kernel_.system_time());
        case 0x82BC5777: // sceKernelGetSystemTimeWide
            state.registers[3] = static_cast<std::uint32_t>(kernel_.system_time() >> 32);
            return static_cast<std::uint32_t>(kernel_.system_time());
        case 0xCEADEB47: // sceKernelDelayThread(a0: microseconds)
            kernel_.delay_thread(arg(0));
            return std::nullopt;
        case 0x19CFF145: // sceKernelCreateLwMutex
            kernel_.create_mutex({.work_area = GuestAddress{arg(0)},
                                  .name = GuestAddress{arg(1)},
                                  .attributes = arg(2),
                                  .initial_count = arg(3),
                                  .options = GuestAddress{arg(4)}});
            return 0;
        case 0x60107536: // sceKernelDeleteLwMutex
            kernel_.delete_mutex(GuestAddress{arg(0)});
            return 0;
        case 0xD6DA4BA1: // sceKernelCreateSema
            return kernel_.create_semaphore({.name = GuestAddress{arg(0)},
                                             .attributes = arg(1),
                                             .initial_count = arg(2),
                                             .maximum = arg(3),
                                             .options = GuestAddress{arg(4)}});
        case 0x28B6489C: // sceKernelDeleteSema
            kernel_.delete_semaphore(arg(0));
            return 0;
        case 0x4E3A1105: // sceKernelWaitSema
            kernel_.wait_semaphore(arg(0), arg(1), GuestAddress{arg(2)});
            return 0;
        case 0x3F53E640: // sceKernelSignalSema
            kernel_.signal_semaphore(arg(0), arg(1));
            return 0;
        default:
            break;
        }
    }
    if (binding.library == "Kernel_Library")
    {
        switch (binding.nid)
        {
        case 0x092968F4: // sceKernelCpuSuspendIntr
            return kernel_.suspend_interrupts();
        case 0x5F10D406: // sceKernelCpuResumeIntr
        case 0x3B84732D: // sceKernelCpuResumeIntrWithSync
            kernel_.resume_interrupts(arg(0));
            return 0;    // Void PSP service; v0 is not part of its contract.
        case 0x47A0B729: // sceKernelIsCpuIntrSuspended (see intr/suspended.expected).
            return static_cast<std::uint32_t>(arg(0) == 0);
        case 0xB55249D2: // sceKernelIsCpuIntrEnable
            return static_cast<std::uint32_t>(kernel_.interrupts_enabled());
        case 0xBEA46419: // sceKernelLockLwMutex
            kernel_.lock_mutex(GuestAddress{arg(0)}, arg(1), GuestAddress{arg(2)});
            return 0;
        case 0x15B6446B: // sceKernelUnlockLwMutex
            kernel_.unlock_mutex(GuestAddress{arg(0)}, arg(1));
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
            return kernel_.largest_free_memory_size();
        case 0xF919F628: // sceKernelTotalFreeMemSize
            return kernel_.free_memory_size();
        case 0x237DBD4F: // sceKernelAllocPartitionMemory
            return kernel_.allocate_partition({.partition = arg(0), .type = arg(2), .size = arg(3)});
        case 0xB6D61D02: // sceKernelFreePartitionMemory
            kernel_.free_partition(arg(0));
            return 0;
        case 0x9D9A5BA1: // sceKernelGetBlockHeadAddr
            return kernel_.block_address(arg(0)).value_of();
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
            io_.device_control({.device = GuestAddress{arg(0)},
                                .command = arg(1),
                                .input = GuestAddress{arg(2)},
                                .input_size = arg(3),
                                .output = GuestAddress{arg(4)},
                                .output_size = arg(5)});
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
    if (binding.library == "sceCtrl")
    {
        switch (binding.nid)
        {
        case 0x6A2774F3: // sceCtrlSetSamplingCycle
            return controller_.set_sampling_cycle(arg(0));
        case 0x1F4011E6: // sceCtrlSetSamplingMode
            return controller_.set_sampling_mode(arg(0));
        case 0x1F803938: // sceCtrlReadBufferPositive
            return controller_.read_positive(GuestAddress{arg(0)}, arg(1));
        default:
            break;
        }
    }
    if (binding.library == "LoadExecForUser")
    {
        switch (binding.nid)
        {
        case 0x05572A5F: // sceKernelExitGame
            kernel_.exit_game();
            return 0;
        case 0x4AC57943: // sceKernelRegisterExitCallback
            kernel_.register_exit_callback(arg(0));
            return 0;
        default:
            break;
        }
    }
    throw std::runtime_error(fmt::format("Unsupported import {}:0x{:x}", binding.library, binding.nid));
}

} // namespace psp::detail
