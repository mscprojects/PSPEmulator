#pragma once

#include "memory/address.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psp
{

class Cpu
{
    // Stage instruction addresses so a failed step preserves pending control flow.
    struct ControlFlow
    {
        GuestAddress next_instruction;
        GuestAddress following_instruction;
    };

    static constexpr std::size_t kRegisterCount = 32;
    static constexpr std::size_t kZeroRegister = 0;           // $zero
    static constexpr std::size_t kReturnAddressRegister = 31; // $ra

public:
    Cpu(Memory &memory, GuestAddress entry_point);

    void step();

    GuestAddress program_counter() const;
    std::uint32_t register_value(std::size_t index) const;

private:
    void execute(std::uint32_t instruction, ControlFlow &flow);
    void execute_special(std::uint32_t instruction, ControlFlow &flow);
    void execute_regimm(std::uint32_t instruction, ControlFlow &flow);
    GuestAddress branch_address(std::uint32_t instruction) const;
    void skip_delay_slot(ControlFlow &flow) const;
    GuestAddress jump_address(std::uint32_t instruction) const;
    GuestAddress word_address(std::uint32_t instruction) const;
    void write_register(std::size_t index, std::uint32_t value);

    Memory &memory_;
    GuestAddress program_counter_;
    GuestAddress next_program_counter_;
    std::array<std::uint32_t, kRegisterCount> registers_{};
};

} // namespace psp
