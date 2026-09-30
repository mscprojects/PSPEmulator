#include "cpu/cpu.hpp"

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace psp
{

Cpu::Cpu(Memory &memory, GuestAddress entry_point)
    : memory_(memory), program_counter_(entry_point), next_program_counter_(entry_point.value_of() + 4U)
{
    if ((entry_point.value_of() & 3U) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
}

void Cpu::step()
{
    if ((program_counter_.value_of() & 3U) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
    const auto instruction = memory_.read_u32(program_counter_);
    ControlFlow flow{next_program_counter_, GuestAddress{next_program_counter_.value_of() + 4U}};
    execute(instruction, flow);
    program_counter_ = flow.next_instruction;
    next_program_counter_ = flow.following_instruction;
}

GuestAddress Cpu::program_counter() const
{
    return program_counter_;
}

std::uint32_t Cpu::register_value(std::size_t index) const
{
    return registers_.at(index);
}

void Cpu::execute(std::uint32_t instruction, ControlFlow &flow)
{
    // Common 32-bit layouts, from most significant to least significant bit:
    // R: opcode[6] | rs[5] | rt[5] | rd[5] | shamt[5] | funct[6]
    // I: opcode[6] | rs[5] | rt[5] | immediate[16]
    // J: opcode[6] | target[26]
    // Brackets give field widths. See docs/cpu-reference.md for operand meanings.
    const auto source = (instruction >> 21) & 0b11111U; // rs: bits 25-21
    const auto target = (instruction >> 16) & 0b11111U; // rt: bits 20-16
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    const auto opcode = instruction >> 26;

    switch (opcode)
    {
    case 0x00: // SPECIAL: the low six function bits select the operation.
        execute_special(instruction, flow);
        break;
    case 0x01: // REGIMM: rt selects the branch operation, rather than a register.
        execute_regimm(instruction, flow);
        break;
    case 0x02: // J
        flow.following_instruction = jump_address(instruction);
        break;
    case 0x03: // JAL
        flow.following_instruction = jump_address(instruction);
        // Return after the delay slot; the link is already visible inside that slot.
        write_register(31, program_counter_.value_of() + 8U);
        break;
    case 0x04: // BEQ
        if (registers_[source] == registers_[target])
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x05: // BNE
        if (registers_[source] != registers_[target])
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x06: // BLEZ
        if (std::bit_cast<std::int32_t>(registers_[source]) <= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x07: // BGTZ
        if (std::bit_cast<std::int32_t>(registers_[source]) > 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x09: // ADDIU
        // The immediate is sign-extended; unsigned addition wraps without an overflow trap.
        write_register(target, registers_[source] + static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0A: // SLTI
        write_register(target, std::bit_cast<std::int32_t>(registers_[source]) < signed_immediate);
        break;
    case 0x0B: // SLTIU
        // Even for an unsigned comparison, the immediate is sign-extended first.
        write_register(target, registers_[source] < static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0C: // ANDI
        // Logical immediates (ANDI, ORI, XORI) are zero-extended instead.
        write_register(target, registers_[source] & immediate);
        break;
    case 0x0D: // ORI
        write_register(target, registers_[source] | immediate);
        break;
    case 0x0E: // XORI
        write_register(target, registers_[source] ^ immediate);
        break;
    case 0x0F: // LUI
        write_register(target, static_cast<std::uint32_t>(immediate) << 16);
        break;
    case 0x14: // BEQL
        if (registers_[source] == registers_[target])
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x15: // BNEL
        if (registers_[source] != registers_[target])
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x16: // BLEZL
        if (std::bit_cast<std::int32_t>(registers_[source]) <= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x17: // BGTZL
        if (std::bit_cast<std::int32_t>(registers_[source]) > 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x23: // LW
        write_register(target, memory_.read_u32(word_address(instruction)));
        break;
    case 0x2B: // SW
        memory_.write_u32(word_address(instruction), registers_[target]);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_special(std::uint32_t instruction, ControlFlow &flow)
{
    const auto source = (instruction >> 21) & 0b11111U;      // rs: bits 25-21
    const auto target = (instruction >> 16) & 0b11111U;      // rt: bits 20-16
    const auto destination = (instruction >> 11) & 0b11111U; // rd: bits 15-11
    const auto shift = (instruction >> 6) & 0b11111U;        // shamt: bits 10-6

    switch (instruction & 0b111111U) // funct: bits 5-0
    {
    case 0x00: // SLL (also NOP)
        write_register(destination, registers_[target] << shift);
        break;
    case 0x02: // SRL
        write_register(destination, registers_[target] >> shift);
        break;
    case 0x03: // SRA
        write_register(destination,
                       std::bit_cast<std::uint32_t>(std::bit_cast<std::int32_t>(registers_[target]) >> shift));
        break;
    case 0x08: // JR
        flow.following_instruction = GuestAddress{registers_[source]};
        break;
    case 0x09: // JALR
        // Capture rs before writing rd, which may be the same register.
        flow.following_instruction = GuestAddress{registers_[source]};
        write_register(destination, program_counter_.value_of() + 8U);
        break;
    case 0x21: // ADDU
        write_register(destination, registers_[source] + registers_[target]);
        break;
    case 0x23: // SUBU
        write_register(destination, registers_[source] - registers_[target]);
        break;
    case 0x24: // AND
        write_register(destination, registers_[source] & registers_[target]);
        break;
    case 0x25: // OR
        write_register(destination, registers_[source] | registers_[target]);
        break;
    case 0x26: // XOR
        write_register(destination, registers_[source] ^ registers_[target]);
        break;
    case 0x27: // NOR
        write_register(destination, ~(registers_[source] | registers_[target]));
        break;
    case 0x2A: // SLT
        write_register(destination, std::bit_cast<std::int32_t>(registers_[source]) <
                                        std::bit_cast<std::int32_t>(registers_[target]));
        break;
    case 0x2B: // SLTU
        write_register(destination, registers_[source] < registers_[target]);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_regimm(std::uint32_t instruction, ControlFlow &flow)
{
    const auto source = (instruction >> 21) & 0b11111U;
    const auto operation = (instruction >> 16) & 0b11111U;
    const auto value = std::bit_cast<std::int32_t>(registers_[source]);

    switch (operation)
    {
    case 0x00: // BLTZ
        if (value < 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x01: // BGEZ
        if (value >= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        break;
    case 0x02: // BLTZL
        if (value < 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x03: // BGEZL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        break;
    case 0x10: // BLTZAL
        if (value < 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        write_register(31, program_counter_.value_of() + 8U);
        break;
    case 0x11: // BGEZAL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        write_register(31, program_counter_.value_of() + 8U);
        break;
    case 0x12: // BLTZALL
        if (value < 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        write_register(31, program_counter_.value_of() + 8U);
        break;
    case 0x13: // BGEZALL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        else
        {
            skip_delay_slot(flow);
        }
        write_register(31, program_counter_.value_of() + 8U);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

GuestAddress Cpu::branch_address(std::uint32_t instruction) const
{
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto offset = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // Branch offsets count words relative to PC + 4. Shift unsigned bits to allow negative offsets.
    return GuestAddress{program_counter_.value_of() + 4U + (static_cast<std::uint32_t>(offset) << 2)};
}

void Cpu::skip_delay_slot(ControlFlow &flow) const
{
    // Untaken branch-likely instructions skip the slot, including its side effects and faults.
    flow.next_instruction = GuestAddress{program_counter_.value_of() + 8U};
    flow.following_instruction = GuestAddress{program_counter_.value_of() + 12U};
}

GuestAddress Cpu::jump_address(std::uint32_t instruction) const
{
    // J/JAL retain the upper four bits of PC + 4 and supply the remaining word address.
    return GuestAddress{((program_counter_.value_of() + 4U) & 0xF0000000U) | ((instruction & 0x03FFFFFFU) << 2)};
}

GuestAddress Cpu::word_address(std::uint32_t instruction) const
{
    const auto source = (instruction >> 21) & 0b11111U; // rs: bits 25-21
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // The offset is signed, but address arithmetic wraps modulo 2^32.
    const auto address = registers_[source] + static_cast<std::uint32_t>(signed_immediate);
    if ((address & 3U) != 0)
    {
        throw std::invalid_argument("Word address must be aligned");
    }
    return GuestAddress{address};
}

void Cpu::write_register(std::size_t index, std::uint32_t value)
{
    // Register zero is hardwired to zero; writes to it are discarded.
    if (index != 0)
    {
        registers_[index] = value;
    }
}

} // namespace psp
