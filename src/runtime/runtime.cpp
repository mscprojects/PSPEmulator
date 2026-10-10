#include "runtime/runtime.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>

namespace psp::detail
{

Runtime::Runtime(const ParsedPrx &prx, const ExecutionOptions &options)
    : loaded_(prepare_prx(prx, options.load_address, options.memory_size)), cpu_(loaded_.memory),
      kernel_(loaded_.memory,
              {loaded_.image_end.value_of(), std::uint64_t{options.load_address.value_of()} + options.memory_size},
              loaded_.module.global_pointer),
      io_(loaded_.memory), display_(loaded_.memory), controller_(loaded_.memory, kernel_),
      ge_(loaded_.memory, kernel_, options.max_instructions),
      dispatcher_(loaded_.memory, kernel_, io_, display_, controller_, ge_, loaded_.imports),
      instruction_budget_(options.max_instructions)
{
    if (instruction_budget_ == 0)
    {
        throw std::invalid_argument("Instruction budget must be positive");
    }
    kernel_.initialize(loaded_.entry_point, options.arguments);
}

ExecutionEvent Runtime::advance()
{
    if (result_)
    {
        return ExecutionEvent::Finished;
    }
    frame_captured_ = false;
    // Guest time moves only here: one microsecond per CPU instruction or GPU-only
    // command, or an idle jump to the next wakeup or vblank. Each vblank is handed
    // to the caller before any later guest work.
    bool vblank = false;
    for (;;)
    {
        if (vblank)
        {
            display_.vblank();
            controller_.vblank();
            return ExecutionEvent::Vblank;
        }
        const auto selection = kernel_.select_next_thread();
        if (selection == ThreadSelection::Finished)
        {
            result_ = ExecutionResult{io_.take_output(), kernel_.exit_code(), instructions_};
            return ExecutionEvent::Finished;
        }
        // One GE command and one CPU instruction can progress in the same guest microsecond.
        const auto gpu_running = ge_.runnable();
        ge_.step();
        const auto interrupt_delivered = ge_.deliver_interrupt();
        if (selection == ThreadSelection::Idle && !interrupt_delivered)
        {
            if (gpu_running)
            {
                vblank = kernel_.advance_time(1);
                continue;
            }
            const auto next_vblank = kernel_.next_vblank_time();
            const auto wakeup = std::min(kernel_.next_wakeup_time().value_or(next_vblank), next_vblank);
            vblank = kernel_.advance_time(wakeup - kernel_.system_time());
            continue;
        }
        auto &state = kernel_.current_thread_state();
        const auto pc = state.program_counter;
        try
        {
            if (instructions_ == instruction_budget_)
            {
                throw std::runtime_error("Instruction budget exhausted");
            }
            kernel_.deliver_pending_interrupt();
            const auto syscall = cpu_.step(state);
            ++instructions_;
            vblank = kernel_.advance_time(1);
            if (syscall)
            {
                dispatcher_.handle(*syscall, state);
            }
        }
        catch (const std::exception &error)
        {
            std::string context = fmt::format("PRX execution at 0x{:x}", pc.value_of());
            try
            {
                context += fmt::format(" (instruction 0x{:x})", loaded_.memory.read_u32(pc));
            }
            catch (const std::out_of_range &)
            {
                // A fetch outside mapped memory has no instruction word to report.
                context += " (instruction unavailable)";
            }
            throw std::runtime_error(context + ": " + error.what());
        }
    }
}

bool Runtime::blocked() const
{
    return !ge_.runnable() && !ge_.interrupt_pending() && kernel_.untimed_waits_only();
}

void Runtime::set_controller(ControllerState input)
{
    controller_.set_input(input);
}

void Runtime::request_exit()
{
    if (!result_)
    {
        kernel_.request_exit();
    }
}

std::uint64_t Runtime::guest_time() const
{
    return kernel_.system_time();
}

std::span<const std::uint8_t> Runtime::pixels()
{
    // Guest memory changes only inside advance(), so one capture serves every read until then.
    if (!frame_captured_)
    {
        display_.capture();
        frame_captured_ = true;
    }
    return display_.pixels();
}

const ExecutionResult &Runtime::result() const
{
    if (!result_)
    {
        throw std::logic_error("Execution has not finished");
    }
    return *result_;
}

std::string_view Runtime::output() const
{
    return result_ ? std::string_view{result_->output} : io_.output();
}

} // namespace psp::detail
