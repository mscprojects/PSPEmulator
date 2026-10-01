#pragma once

#include "memory/address.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

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

    enum class DataAlignment : std::uint8_t
    {
        Byte = 1,
        Halfword = 2,
        Word = 4,
    };

    static constexpr std::size_t kRegisterCount = 32;
    static constexpr std::size_t kZeroRegister = 0;           // $zero
    static constexpr std::size_t kReturnAddressRegister = 31; // $ra

public:
    Cpu(Memory &memory, GuestAddress entry_point);

    // Execute one instruction. A SYSCALL reports its code after committing control
    // flow; the runtime handles it before executing the next instruction.
    std::optional<std::uint32_t> step();

    GuestAddress program_counter() const;
    std::uint32_t register_value(std::size_t index) const;
    // Initialize registers and supply service results; writes to $zero are discarded.
    void set_register_value(std::size_t index, std::uint32_t value);

private:
    void execute(std::uint32_t instruction, ControlFlow &flow);
    void execute_special(std::uint32_t instruction, ControlFlow &flow);
    void execute_special3(std::uint32_t instruction);
    void execute_regimm(std::uint32_t instruction, ControlFlow &flow);
    GuestAddress branch_address(std::uint32_t instruction) const;
    void skip_delay_slot(ControlFlow &flow) const;
    GuestAddress jump_address(std::uint32_t instruction) const;
    GuestAddress data_address(std::uint32_t instruction, DataAlignment alignment) const;
    std::uint64_t hi_lo_value() const;
    void write_hi_lo(std::uint64_t value);
    void write_register(std::size_t index, std::uint32_t value);

    Memory &memory_;
    GuestAddress program_counter_;
    GuestAddress next_program_counter_;
    std::array<std::uint32_t, kRegisterCount> registers_{};
    std::uint32_t high_register_{};
    std::uint32_t low_register_{};
};

} // namespace psp
