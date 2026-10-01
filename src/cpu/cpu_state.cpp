#include "cpu/cpu_state.hpp"

#include <stdexcept>

namespace psp
{

CpuState::CpuState(GuestAddress entry_point)
    : program_counter_(entry_point), next_program_counter_(entry_point.value_of() + 4)
{
    if ((entry_point.value_of() & 0b11U) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
}

GuestAddress CpuState::program_counter() const
{
    return program_counter_;
}

std::uint32_t CpuState::register_value(std::size_t index) const
{
    return registers_.at(index);
}

void CpuState::set_register_value(std::size_t index, std::uint32_t value)
{
    if (index >= registers_.size())
    {
        throw std::out_of_range("CPU register index is out of range");
    }
    if (index != 0)
    {
        registers_[index] = value;
    }
}

} // namespace psp
