#include "cpu/cpu.hpp"

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace psp
{

Cpu::Cpu(Memory &memory, GuestAddress entry_point) : memory_(memory), program_counter_(entry_point)
{
    if ((entry_point.value_of() & 3U) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
}

void Cpu::step()
{
    const auto instruction = memory_.read_u32(program_counter_);
    execute(instruction);
    program_counter_ = GuestAddress{program_counter_.value_of() + 4U};
}

GuestAddress Cpu::program_counter() const
{
    return program_counter_;
}

std::uint32_t Cpu::register_value(std::size_t index) const
{
    return registers_.at(index);
}

void Cpu::execute(std::uint32_t instruction)
{
    const auto source = (instruction >> 21) & 31U;
    const auto target = (instruction >> 16) & 31U;
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    const auto opcode = instruction >> 26;

    switch (opcode)
    {
    case 0x00: // SPECIAL
        execute_special(instruction);
        break;
    case 0x09: // ADDIU
        write_register(target, registers_[source] + static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0A: // SLTI
        write_register(target, std::bit_cast<std::int32_t>(registers_[source]) < signed_immediate);
        break;
    case 0x0B: // SLTIU
        write_register(target, registers_[source] < static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0C: // ANDI
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
    case 0x23: // LW
    case 0x2B: // SW
    {
        const auto address = registers_[source] + static_cast<std::uint32_t>(signed_immediate);
        if ((address & 3U) != 0)
        {
            throw std::invalid_argument("Word address must be aligned");
        }
        if (opcode == 0x23)
        {
            write_register(target, memory_.read_u32(GuestAddress{address}));
        }
        else
        {
            memory_.write_u32(GuestAddress{address}, registers_[target]);
        }
        break;
    }
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_special(std::uint32_t instruction)
{
    const auto source = (instruction >> 21) & 31U;
    const auto target = (instruction >> 16) & 31U;
    const auto destination = (instruction >> 11) & 31U;
    const auto shift = (instruction >> 6) & 31U;

    switch (instruction & 63U)
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

void Cpu::write_register(std::size_t index, std::uint32_t value)
{
    if (index != 0)
    {
        registers_[index] = value;
    }
}

} // namespace psp
