#pragma once

#include "cpu/cpu_state.hpp"
#include "memory/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace psp
{

// Instruction event for the runtime's HLE dispatcher, not a service result or NID.
struct Syscall
{
    std::uint32_t code;
    GuestAddress instruction_address;
    bool operator==(const Syscall &) const = default;
};

// Shared instruction interpreter; architectural state is supplied for each step.
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

    static constexpr std::size_t kReturnAddressRegister = 31; // $ra

public:
    explicit Cpu(Memory &memory);

    // Execute one instruction. A SYSCALL reports its code after committing control
    // flow; the runtime handles it before executing the next instruction.
    std::optional<Syscall> step(CpuState &state);

private:
    void execute(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    void execute_special(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    void execute_special3(CpuState &state, std::uint32_t instruction);
    void execute_regimm(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    GuestAddress branch_address(const CpuState &state, std::uint32_t instruction) const;
    void skip_delay_slot(const CpuState &state, ControlFlow &flow) const;
    GuestAddress jump_address(const CpuState &state, std::uint32_t instruction) const;
    GuestAddress data_address(const CpuState &state, std::uint32_t instruction, DataAlignment alignment) const;

    std::uint64_t hi_lo_value(const CpuState &state) const;
    void write_hi_lo(CpuState &state, std::uint64_t value);
    void write_register(CpuState &state, std::size_t index, std::uint32_t value);

    Memory &memory_;
};

} // namespace psp
