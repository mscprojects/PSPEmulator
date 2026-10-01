#pragma once

#include "memory/address.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psp
{

struct InstructionAddresses
{
    GuestAddress current;
    GuestAddress next;
};

// Architectural state for one guest thread. Copies preserve the pending delay-slot
// target as well as registers; memory belongs to the runtime and is shared.
class CpuState
{
public:
    explicit CpuState(GuestAddress entry_point);

    GuestAddress program_counter() const;
    GuestAddress next_program_counter() const;
    // Commit both addresses together after a successful instruction. Alignment is
    // checked at fetch, so a register jump can fault after executing its delay slot.
    void set_instruction_addresses(InstructionAddresses addresses);

    std::uint32_t register_value(std::size_t index) const;
    // Initialize registers and supply service results; writes to $zero are discarded.
    void set_register_value(std::size_t index, std::uint32_t value);

    std::uint32_t high_register_value() const;
    void set_high_register_value(std::uint32_t value);
    std::uint32_t low_register_value() const;
    void set_low_register_value(std::uint32_t value);
    // HI/LO form a 64-bit accumulator for multiply and accumulate instructions.
    std::uint64_t hi_lo_value() const;
    void set_hi_lo_value(std::uint64_t value);

private:
    GuestAddress program_counter_;
    GuestAddress next_program_counter_;
    std::array<std::uint32_t, 32> registers_{};
    std::uint32_t high_register_{};
    std::uint32_t low_register_{};
};

} // namespace psp
