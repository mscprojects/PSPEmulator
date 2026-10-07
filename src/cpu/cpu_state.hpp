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
    // Raw IEEE-754 binary32 words preserve all transfer payloads.
    std::array<std::uint32_t, 32> floating_point_registers{};
    std::uint32_t floating_point_control{0x00000E00}; // PSP thread startup FCR31.
    // Comparisons reach branches one instruction later; CFC1 sees FCR31 immediately.
    bool floating_point_branch_condition{};
    // Allegrex LL/SC uses a link bit, without tracking a reserved address.
    bool load_linked{};

    bool operator==(const CpuState &) const = default;
};

} // namespace psp
