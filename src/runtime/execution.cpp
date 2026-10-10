#include "runtime/execution.hpp"

#include "runtime/runtime.hpp"

#include <stdexcept>

namespace psp
{

Execution::Execution(const ParsedPrx &prx, const ExecutionOptions &options)
    : runtime_(std::make_unique<detail::Runtime>(prx, options))
{
}

Execution::~Execution() = default;

ExecutionEvent Execution::advance()
{
    return runtime_->advance();
}

void Execution::run_to_completion()
{
    while (runtime_->advance() != ExecutionEvent::Finished)
    {
        if (runtime_->blocked())
        {
            throw std::runtime_error("Guest is blocked: no thread can run or wake without host input");
        }
    }
}

void Execution::set_controller(ControllerState input)
{
    runtime_->set_controller(input);
}

void Execution::request_exit()
{
    runtime_->request_exit();
}

std::uint64_t Execution::guest_time() const
{
    return runtime_->guest_time();
}

std::span<const std::uint8_t> Execution::pixels()
{
    return runtime_->pixels();
}

const ExecutionResult &Execution::result() const
{
    return runtime_->result();
}

std::string_view Execution::output() const
{
    return runtime_->output();
}

ExecutionResult execute_prx(const ParsedPrx &prx, const ExecutionOptions &options)
{
    Execution execution(prx, options);
    execution.run_to_completion();
    return execution.result();
}

} // namespace psp
