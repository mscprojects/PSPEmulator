#pragma once

#include "memory/address.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psp
{

// Architectural state for one guest thread. Copies preserve the pending delay-slot
// target as well as registers; memory belongs to the runtime and is shared.
class CpuState
{
    friend class Cpu;

public:
    explicit CpuState(GuestAddress entry_point);

    GuestAddress program_counter() const;
    std::uint32_t register_value(std::size_t index) const;
    // Initialize registers and supply service results; writes to $zero are discarded.
    void set_register_value(std::size_t index, std::uint32_t value);

private:
    GuestAddress program_counter_;
    GuestAddress next_program_counter_;
    std::array<std::uint32_t, 32> registers_{};
    std::uint32_t high_register_{};
    std::uint32_t low_register_{};
};

} // namespace psp
