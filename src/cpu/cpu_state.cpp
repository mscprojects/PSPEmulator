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

GuestAddress CpuState::next_program_counter() const
{
    return next_program_counter_;
}

void CpuState::set_instruction_addresses(InstructionAddresses addresses)
{
    program_counter_ = addresses.current;
    next_program_counter_ = addresses.next;
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

std::uint32_t CpuState::high_register_value() const
{
    return high_register_;
}

void CpuState::set_high_register_value(std::uint32_t value)
{
    high_register_ = value;
}

std::uint32_t CpuState::low_register_value() const
{
    return low_register_;
}

void CpuState::set_low_register_value(std::uint32_t value)
{
    low_register_ = value;
}

std::uint64_t CpuState::hi_lo_value() const
{
    return (static_cast<std::uint64_t>(high_register_) << 32) | low_register_;
}

void CpuState::set_hi_lo_value(std::uint64_t value)
{
    low_register_ = static_cast<std::uint32_t>(value);
    high_register_ = static_cast<std::uint32_t>(value >> 32);
}

} // namespace psp
