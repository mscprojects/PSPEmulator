#include "cpu/cpu.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace psp
{

namespace
{

constexpr std::uint32_t kInstructionSize = 4;
constexpr std::uint32_t kAfterDelaySlotOffset = 2 * kInstructionSize;
constexpr std::uint32_t kWordAlignmentMask = 0b11U;
constexpr std::uint32_t kJumpRegionMask = 0xF0000000U;
constexpr std::uint32_t kJumpTargetMask = 0x03FFFFFFU;
constexpr unsigned kRegisterBits = 32;
constexpr std::uint32_t kShiftCountMask = 0b11111U;
constexpr std::uint32_t kLowHalfwordMask = 0xFFFFU;
constexpr std::uint32_t kEvenByteMask = 0x00FF00FFU;
constexpr std::uint32_t kOddByteMask = 0xFF00FF00U;

std::uint64_t signed_product(std::uint32_t left, std::uint32_t right)
{
    const auto product = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(left)) *
                         static_cast<std::int64_t>(std::bit_cast<std::int32_t>(right));
    // Preserve the signed product's bits for wrapping HI/LO accumulation.
    return static_cast<std::uint64_t>(product);
}

} // namespace

Cpu::Cpu(Memory &memory) : memory_(memory)
{
}

std::optional<std::uint32_t> Cpu::step(CpuState &state)
{
    if ((state.program_counter().value_of() & kWordAlignmentMask) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
    const auto instruction = memory_.read_u32(state.program_counter());
    ControlFlow flow{state.next_program_counter(),
                     GuestAddress{state.next_program_counter().value_of() + kInstructionSize}};
    std::optional<std::uint32_t> syscall;
    if ((instruction & 0xFC00003FU) == 0x0000000CU)
    {
        syscall = (instruction >> 6) & 0xFFFFFU;
    }
    else
    {
        execute(state, instruction, flow);
    }
    state.set_instruction_addresses({flow.next_instruction, flow.following_instruction});
    return syscall;
}

void Cpu::execute(CpuState &state, std::uint32_t instruction, ControlFlow &flow)
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
        execute_special(state, instruction, flow);
        break;
    case 0x01: // REGIMM: rt selects the branch operation, rather than a register.
        execute_regimm(state, instruction, flow);
        break;
    case 0x02: // J
        flow.following_instruction = jump_address(state, instruction);
        break;
    case 0x03: // JAL
        flow.following_instruction = jump_address(state, instruction);
        // Return after the delay slot; the link is already visible inside that slot.
        state.set_register_value(kReturnAddressRegister, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    case 0x04: // BEQ
        if (state.register_value(source) == state.register_value(target))
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x05: // BNE
        if (state.register_value(source) != state.register_value(target))
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x06: // BLEZ
        if (std::bit_cast<std::int32_t>(state.register_value(source)) <= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x07: // BGTZ
        if (std::bit_cast<std::int32_t>(state.register_value(source)) > 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x09: // ADDIU
        // The immediate is sign-extended; unsigned addition wraps without an overflow trap.
        state.set_register_value(target, state.register_value(source) + static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0A: // SLTI
        state.set_register_value(target, std::bit_cast<std::int32_t>(state.register_value(source)) < signed_immediate);
        break;
    case 0x0B: // SLTIU
        // Even for an unsigned comparison, the immediate is sign-extended first.
        state.set_register_value(target, state.register_value(source) < static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0C: // ANDI
        // Logical immediates (ANDI, ORI, XORI) are zero-extended instead.
        state.set_register_value(target, state.register_value(source) & immediate);
        break;
    case 0x0D: // ORI
        state.set_register_value(target, state.register_value(source) | immediate);
        break;
    case 0x0E: // XORI
        state.set_register_value(target, state.register_value(source) ^ immediate);
        break;
    case 0x0F: // LUI
        state.set_register_value(target, static_cast<std::uint32_t>(immediate) << 16);
        break;
    case 0x14: // BEQL
        if (state.register_value(source) == state.register_value(target))
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x15: // BNEL
        if (state.register_value(source) != state.register_value(target))
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x16: // BLEZL
        if (std::bit_cast<std::int32_t>(state.register_value(source)) <= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x17: // BGTZL
        if (std::bit_cast<std::int32_t>(state.register_value(source)) > 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x1F: // SPECIAL3: bitfields and Allegrex byte/bit operations.
        execute_special3(state, instruction);
        break;
    case 0x20: // LB
        state.set_register_value(target, static_cast<std::uint32_t>(std::bit_cast<std::int8_t>(
                                             memory_.read_u8(data_address(state, instruction, DataAlignment::Byte)))));
        break;
    case 0x21: // LH
        state.set_register_value(target, static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(memory_.read_u16(
                                             data_address(state, instruction, DataAlignment::Halfword)))));
        break;
    case 0x23: // LW
        state.set_register_value(target, memory_.read_u32(data_address(state, instruction, DataAlignment::Word)));
        break;
    case 0x24: // LBU
        state.set_register_value(target, memory_.read_u8(data_address(state, instruction, DataAlignment::Byte)));
        break;
    case 0x25: // LHU
        state.set_register_value(target, memory_.read_u16(data_address(state, instruction, DataAlignment::Halfword)));
        break;
    case 0x28: // SB
        memory_.write_u8(data_address(state, instruction, DataAlignment::Byte),
                         static_cast<std::uint8_t>(state.register_value(target)));
        break;
    case 0x29: // SH
        memory_.write_u16(data_address(state, instruction, DataAlignment::Halfword),
                          static_cast<std::uint16_t>(state.register_value(target)));
        break;
    case 0x2B: // SW
        memory_.write_u32(data_address(state, instruction, DataAlignment::Word), state.register_value(target));
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_special(CpuState &state, std::uint32_t instruction, ControlFlow &flow)
{
    const auto source = (instruction >> 21) & 0b11111U;      // rs: bits 25-21
    const auto target = (instruction >> 16) & 0b11111U;      // rt: bits 20-16
    const auto destination = (instruction >> 11) & 0b11111U; // rd: bits 15-11
    const auto shift = (instruction >> 6) & 0b11111U;        // shamt: bits 10-6

    switch (instruction & 0b111111U) // funct: bits 5-0
    {
    case 0x00: // SLL (also NOP)
        state.set_register_value(destination, state.register_value(target) << shift);
        break;
    case 0x02: // SRL / ROTR: rs is an operation selector here.
        if (source == 0)
        {
            state.set_register_value(destination, state.register_value(target) >> shift);
        }
        else if (source == 1)
        {
            state.set_register_value(destination, std::rotr(state.register_value(target), static_cast<int>(shift)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x03: // SRA
        state.set_register_value(destination, std::bit_cast<std::uint32_t>(
                                                  std::bit_cast<std::int32_t>(state.register_value(target)) >> shift));
        break;
    case 0x04: // SLLV: variable shifts use only the low five count bits.
        state.set_register_value(destination, state.register_value(target)
                                                  << (state.register_value(source) & kShiftCountMask));
        break;
    case 0x06: // SRLV / ROTRV: shamt selects shift or rotate.
        if (shift == 0)
        {
            state.set_register_value(destination,
                                     state.register_value(target) >> (state.register_value(source) & kShiftCountMask));
        }
        else if (shift == 1)
        {
            state.set_register_value(destination,
                                     std::rotr(state.register_value(target),
                                               static_cast<int>(state.register_value(source) & kShiftCountMask)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x07: // SRAV
        state.set_register_value(destination,
                                 static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(state.register_value(target)) >>
                                                            (state.register_value(source) & kShiftCountMask)));
        break;
    case 0x08: // JR
        flow.following_instruction = GuestAddress{state.register_value(source)};
        break;
    case 0x09: // JALR
        // Capture rs before writing rd, which may be the same register.
        flow.following_instruction = GuestAddress{state.register_value(source)};
        state.set_register_value(destination, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    case 0x0A: // MOVZ
        if (state.register_value(target) == 0)
        {
            state.set_register_value(destination, state.register_value(source));
        }
        break;
    case 0x0B: // MOVN
        if (state.register_value(target) != 0)
        {
            state.set_register_value(destination, state.register_value(source));
        }
        break;
    case 0x10: // MFHI
        state.set_register_value(destination, state.high_register_value());
        break;
    case 0x11: // MTHI
        state.set_high_register_value(state.register_value(source));
        break;
    case 0x12: // MFLO
        state.set_register_value(destination, state.low_register_value());
        break;
    case 0x13: // MTLO
        state.set_low_register_value(state.register_value(source));
        break;
    case 0x16: // CLZ (Allegrex uses SPECIAL, rather than generic MIPS32 SPECIAL2).
        state.set_register_value(destination,
                                 static_cast<std::uint32_t>(std::countl_zero(state.register_value(source))));
        break;
    case 0x17: // CLO
        state.set_register_value(destination,
                                 static_cast<std::uint32_t>(std::countl_one(state.register_value(source))));
        break;
    case 0x18: // MULT
        state.set_hi_lo_value(signed_product(state.register_value(source), state.register_value(target)));
        break;
    case 0x19: // MULTU
        state.set_hi_lo_value(static_cast<std::uint64_t>(state.register_value(source)) * state.register_value(target));
        break;
    case 0x1A: // DIV
    {
        const auto numerator = std::bit_cast<std::int32_t>(state.register_value(source));
        const auto denominator = std::bit_cast<std::int32_t>(state.register_value(target));
        // Handle Allegrex's special results before host division, which would be undefined.
        if (denominator == 0)
        {
            state.set_low_register_value(numerator < 0 ? 1U : 0xFFFFFFFFU);
            state.set_high_register_value(state.register_value(source));
        }
        else if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1)
        {
            state.set_low_register_value(state.register_value(source));
            state.set_high_register_value(0);
        }
        else
        {
            state.set_low_register_value(static_cast<std::uint32_t>(numerator / denominator));
            state.set_high_register_value(static_cast<std::uint32_t>(numerator % denominator));
        }
        break;
    }
    case 0x1B: // DIVU
        if (state.register_value(target) == 0)
        {
            // Hardware returns a 16-bit quotient for small numerators; see cpu_div.expected.
            state.set_low_register_value(state.register_value(source) <= kLowHalfwordMask ? kLowHalfwordMask
                                                                                          : 0xFFFFFFFFU);
            state.set_high_register_value(state.register_value(source));
        }
        else
        {
            state.set_low_register_value(state.register_value(source) / state.register_value(target));
            state.set_high_register_value(state.register_value(source) % state.register_value(target));
        }
        break;
    case 0x1C: // MADD (Allegrex SPECIAL encoding)
        state.set_hi_lo_value(state.hi_lo_value() +
                              signed_product(state.register_value(source), state.register_value(target)));
        break;
    case 0x1D: // MADDU
        state.set_hi_lo_value(state.hi_lo_value() +
                              static_cast<std::uint64_t>(state.register_value(source)) * state.register_value(target));
        break;
    case 0x21: // ADDU
        state.set_register_value(destination, state.register_value(source) + state.register_value(target));
        break;
    case 0x23: // SUBU
        state.set_register_value(destination, state.register_value(source) - state.register_value(target));
        break;
    case 0x24: // AND
        state.set_register_value(destination, state.register_value(source) & state.register_value(target));
        break;
    case 0x25: // OR
        state.set_register_value(destination, state.register_value(source) | state.register_value(target));
        break;
    case 0x26: // XOR
        state.set_register_value(destination, state.register_value(source) ^ state.register_value(target));
        break;
    case 0x27: // NOR
        state.set_register_value(destination, ~(state.register_value(source) | state.register_value(target)));
        break;
    case 0x2A: // SLT
        state.set_register_value(destination, std::bit_cast<std::int32_t>(state.register_value(source)) <
                                                  std::bit_cast<std::int32_t>(state.register_value(target)));
        break;
    case 0x2B: // SLTU
        state.set_register_value(destination, state.register_value(source) < state.register_value(target));
        break;
    case 0x2C: // MAX: operands are signed.
        state.set_register_value(destination, static_cast<std::uint32_t>(
                                                  std::max(std::bit_cast<std::int32_t>(state.register_value(source)),
                                                           std::bit_cast<std::int32_t>(state.register_value(target)))));
        break;
    case 0x2D: // MIN
        state.set_register_value(destination, static_cast<std::uint32_t>(
                                                  std::min(std::bit_cast<std::int32_t>(state.register_value(source)),
                                                           std::bit_cast<std::int32_t>(state.register_value(target)))));
        break;
    case 0x2E: // MSUB
        state.set_hi_lo_value(state.hi_lo_value() -
                              signed_product(state.register_value(source), state.register_value(target)));
        break;
    case 0x2F: // MSUBU
        state.set_hi_lo_value(state.hi_lo_value() -
                              static_cast<std::uint64_t>(state.register_value(source)) * state.register_value(target));
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_special3(CpuState &state, std::uint32_t instruction)
{
    const auto source = (instruction >> 21) & 0b11111U;
    const auto target = (instruction >> 16) & 0b11111U;
    const auto destination = (instruction >> 11) & 0b11111U;
    const auto position = (instruction >> 6) & 0b11111U;

    switch (instruction & 0b111111U)
    {
    case 0x00: // EXT: rd encodes width - 1; rt is the destination register.
    {
        const auto width = destination + 1;
        if (position + width > kRegisterBits)
        {
            throw std::invalid_argument("Bitfield exceeds register width");
        }
        // A 64-bit mask permits a full 32-bit field without shifting a word by 32.
        const auto mask = static_cast<std::uint32_t>((std::uint64_t{1} << width) - 1);
        state.set_register_value(target, (state.register_value(source) >> position) & mask);
        break;
    }
    case 0x04: // INS: rd encodes the highest destination bit, rather than width.
    {
        if (destination < position)
        {
            throw std::invalid_argument("Bitfield end precedes its start");
        }
        const auto width = destination - position + 1;
        const auto mask = static_cast<std::uint32_t>((std::uint64_t{1} << width) - 1) << position;
        state.set_register_value(target, (state.register_value(target) & ~mask) |
                                             ((state.register_value(source) << position) & mask));
        break;
    }
    case 0x20: // BSHFL: shamt selects the byte/bit operation.
        switch (position)
        {
        case 0x02: // WSBH
            state.set_register_value(destination, ((state.register_value(target) & kOddByteMask) >> 8) |
                                                      ((state.register_value(target) & kEvenByteMask) << 8));
            break;
        case 0x03: // WSBW
            state.set_register_value(destination, std::byteswap(state.register_value(target)));
            break;
        case 0x10: // SEB
            state.set_register_value(destination, static_cast<std::uint32_t>(std::bit_cast<std::int8_t>(
                                                      static_cast<std::uint8_t>(state.register_value(target)))));
            break;
        case 0x14: // BITREV
        {
            auto value = state.register_value(target);
            std::uint32_t result = 0;
            for (unsigned bit = 0; bit < kRegisterBits; ++bit)
            {
                result = (result << 1) | (value & 1U);
                value >>= 1;
            }
            state.set_register_value(destination, result);
            break;
        }
        case 0x18: // SEH
            state.set_register_value(destination, static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(
                                                      static_cast<std::uint16_t>(state.register_value(target)))));
            break;
        default:
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_regimm(CpuState &state, std::uint32_t instruction, ControlFlow &flow)
{
    const auto source = (instruction >> 21) & 0b11111U;
    const auto operation = (instruction >> 16) & 0b11111U;
    const auto value = std::bit_cast<std::int32_t>(state.register_value(source));

    switch (operation)
    {
    case 0x00: // BLTZ
        if (value < 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x01: // BGEZ
        if (value >= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x02: // BLTZL
        if (value < 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x03: // BGEZL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x10: // BLTZAL
        if (value < 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        state.set_register_value(kReturnAddressRegister, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    case 0x11: // BGEZAL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        state.set_register_value(kReturnAddressRegister, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    case 0x12: // BLTZALL
        if (value < 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        state.set_register_value(kReturnAddressRegister, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    case 0x13: // BGEZALL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        state.set_register_value(kReturnAddressRegister, state.program_counter().value_of() + kAfterDelaySlotOffset);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

GuestAddress Cpu::branch_address(const CpuState &state, std::uint32_t instruction) const
{
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto offset = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // Branch offsets count instructions relative to PC + 4. Unsigned arithmetic permits wrapping backwards.
    return GuestAddress{state.program_counter().value_of() + kInstructionSize +
                        static_cast<std::uint32_t>(offset) * kInstructionSize};
}

void Cpu::skip_delay_slot(const CpuState &state, ControlFlow &flow) const
{
    // Untaken branch-likely instructions skip the slot, including its side effects and faults.
    flow.next_instruction = GuestAddress{state.program_counter().value_of() + kAfterDelaySlotOffset};
    flow.following_instruction = GuestAddress{flow.next_instruction.value_of() + kInstructionSize};
}

GuestAddress Cpu::jump_address(const CpuState &state, std::uint32_t instruction) const
{
    // J/JAL retain the upper four bits of PC + 4 and supply the remaining word address.
    return GuestAddress{((state.program_counter().value_of() + kInstructionSize) & kJumpRegionMask) |
                        ((instruction & kJumpTargetMask) * kInstructionSize)};
}

GuestAddress Cpu::data_address(const CpuState &state, std::uint32_t instruction, DataAlignment alignment) const
{
    const auto source = (instruction >> 21) & 0b11111U; // rs: bits 25-21
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // The offset is signed, but address arithmetic wraps modulo 2^32.
    const auto address = state.register_value(source) + static_cast<std::uint32_t>(signed_immediate);
    if (address % static_cast<std::uint32_t>(alignment) != 0)
    {
        throw std::invalid_argument("Data address must be aligned");
    }
    return GuestAddress{address};
}

} // namespace psp
