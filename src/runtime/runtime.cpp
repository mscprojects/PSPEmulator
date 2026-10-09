#include "runtime/runtime.hpp"

#include <fmt/format.h>

#include <stdexcept>

namespace psp::detail
{

Runtime::Runtime(const ParsedPrx &prx, const ExecutionOptions &options)
    : loaded_(prepare_prx(prx, options.load_address, options.memory_size)), cpu_(loaded_.memory),
      kernel_(loaded_.memory, options.load_address, options.memory_size, prx.segments, loaded_.module.global_pointer),
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
    next_vblank_ = kernel_.next_vblank_time();
}

ExecutionEvent Runtime::advance()
{
    if (result_)
    {
        return ExecutionEvent::Finished;
    }
    for (;;)
    {
        if (kernel_.system_time() == next_vblank_)
        {
            display_.vblank();
            controller_.vblank();
            next_vblank_ = kernel_.next_vblank_time();
            return ExecutionEvent::Vblank;
        }
        const auto gpu_running = ge_.runnable();
        const auto selection = kernel_.select_next_thread(gpu_running ? kernel_.system_time() : next_vblank_);
        if (selection == ThreadSelection::Finished)
        {
            display_.capture();
            result_ = ExecutionResult{io_.take_output(), kernel_.exit_code(), instructions_};
            return ExecutionEvent::Finished;
        }
        // Selection can advance an idle clock exactly to a vblank/wakeup tie.
        if (kernel_.system_time() == next_vblank_)
        {
            continue;
        }
        // One GE command and one CPU instruction can progress in the same guest
        // microsecond. GPU-only work advances time without consuming CPU instructions.
        ge_.step();
        if (selection == ThreadSelection::Idle)
        {
            if (gpu_running)
            {
                kernel_.advance_time(1);
            }
            continue;
        }
        ge_.deliver_interrupt();
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
            kernel_.advance_time(1);
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

std::span<const std::uint8_t> Runtime::pixels() const
{
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
