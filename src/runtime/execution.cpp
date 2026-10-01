#include "runtime/execution.hpp"

#include "runtime/runtime.hpp"

namespace psp
{

ExecutionResult execute_prx(const ParsedPrx &prx, const ExecutionOptions &options)
{
    return detail::Runtime(prx, options).run();
}

} // namespace psp
