#pragma once

#include "cpu/cpu_state.hpp"
#include "memory/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace psp
{

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
    std::optional<std::uint32_t> step(CpuState &state);

private:
    void execute(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    void execute_special(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    void execute_special3(CpuState &state, std::uint32_t instruction);
    void execute_regimm(CpuState &state, std::uint32_t instruction, ControlFlow &flow);
    GuestAddress branch_address(const CpuState &state, std::uint32_t instruction) const;
    void skip_delay_slot(const CpuState &state, ControlFlow &flow) const;
    GuestAddress jump_address(const CpuState &state, std::uint32_t instruction) const;
    GuestAddress data_address(const CpuState &state, std::uint32_t instruction, DataAlignment alignment) const;

    Memory &memory_;
};

} // namespace psp
