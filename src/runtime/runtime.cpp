#include "runtime/runtime.hpp"

#include <fmt/format.h>

#include <stdexcept>

namespace psp::detail
{

Runtime::Runtime(const ParsedPrx &prx, const ExecutionOptions &options)
    : loaded_(prepare_prx(prx, options.load_address, options.memory_size)), cpu_(loaded_.memory),
      allocator_(options.load_address, options.memory_size, prx.segments),
      kernel_(loaded_.memory, allocator_, loaded_.module.global_pointer), io_(loaded_.memory),
      dispatcher_(loaded_.memory, kernel_, allocator_, io_, loaded_.imports),
      instruction_budget_(options.max_instructions)
{
    if (instruction_budget_ == 0)
    {
        throw std::invalid_argument("Instruction budget must be positive");
    }
    kernel_.threads.initialize(loaded_.entry_point, options.arguments);
}

ExecutionResult Runtime::run()
{
    while (kernel_.threads.select_next())
    {
        auto &state = kernel_.threads.current_state();
        const auto pc = state.program_counter;
        try
        {
            if (instructions_ == instruction_budget_)
            {
                throw std::runtime_error("Instruction budget exhausted");
            }
            const auto syscall = cpu_.step(state);
            ++instructions_;
            if (syscall)
            {
                dispatcher_.handle(*syscall, state, instructions_);
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
    return {io_.take_output(), kernel_.threads.exit_code(), instructions_};
}

} // namespace psp::detail
