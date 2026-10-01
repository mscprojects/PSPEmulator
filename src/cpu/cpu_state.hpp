#pragma once

#include "memory/address.hpp"

#include <array>
#include <cstdint>

namespace psp
{

// Saved architectural state for one guest thread, including any pending delay-slot
// target. Memory is shared separately. The interpreter enforces $zero at each step.
struct CpuState
{
    GuestAddress program_counter{0};
    GuestAddress next_program_counter{program_counter.value_of() + 4};
    std::array<std::uint32_t, 32> registers{};
    std::uint32_t high_register{};
    std::uint32_t low_register{};

    bool operator==(const CpuState &) const = default;
};

} // namespace psp
