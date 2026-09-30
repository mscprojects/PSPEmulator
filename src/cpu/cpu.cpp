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
constexpr unsigned kByteBits = 8;
constexpr std::uint32_t kShiftCountMask = 0b11111U;
constexpr std::uint32_t kLowHalfwordMask = 0xFFFFU;
constexpr std::uint32_t kAllWordBits = std::numeric_limits<std::uint32_t>::max();
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

Cpu::Cpu(Memory &memory, GuestAddress entry_point)
    : memory_(memory), program_counter_(entry_point), next_program_counter_(entry_point.value_of() + kInstructionSize)
{
    if ((entry_point.value_of() & kWordAlignmentMask) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
}

void Cpu::step()
{
    if ((program_counter_.value_of() & kWordAlignmentMask) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
    const auto instruction = memory_.read_u32(program_counter_);
    ControlFlow flow{next_program_counter_, GuestAddress{next_program_counter_.value_of() + kInstructionSize}};
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
        write_register(kReturnAddressRegister, program_counter_.value_of() + kAfterDelaySlotOffset);
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
    case 0x1F: // SPECIAL3: bitfields and Allegrex byte/bit operations.
        execute_special3(instruction);
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
    case 0x02: // SRL / ROTR: rs is an operation selector here.
        if (source == 0)
        {
            write_register(destination, registers_[target] >> shift);
        }
        else if (source == 1)
        {
            write_register(destination, std::rotr(registers_[target], static_cast<int>(shift)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x03: // SRA
        write_register(destination,
                       std::bit_cast<std::uint32_t>(std::bit_cast<std::int32_t>(registers_[target]) >> shift));
        break;
    case 0x04: // SLLV: variable shifts use only the low five count bits.
        write_register(destination, registers_[target] << (registers_[source] & kShiftCountMask));
        break;
    case 0x06: // SRLV / ROTRV: shamt selects shift or rotate.
        if (shift == 0)
        {
            write_register(destination, registers_[target] >> (registers_[source] & kShiftCountMask));
        }
        else if (shift == 1)
        {
            write_register(destination,
                           std::rotr(registers_[target], static_cast<int>(registers_[source] & kShiftCountMask)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x07: // SRAV
        write_register(destination, static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(registers_[target]) >>
                                                               (registers_[source] & kShiftCountMask)));
        break;
    case 0x08: // JR
        flow.following_instruction = GuestAddress{registers_[source]};
        break;
    case 0x09: // JALR
        // Capture rs before writing rd, which may be the same register.
        flow.following_instruction = GuestAddress{registers_[source]};
        write_register(destination, program_counter_.value_of() + kAfterDelaySlotOffset);
        break;
    case 0x0A: // MOVZ
        if (registers_[target] == 0)
        {
            write_register(destination, registers_[source]);
        }
        break;
    case 0x0B: // MOVN
        if (registers_[target] != 0)
        {
            write_register(destination, registers_[source]);
        }
        break;
    case 0x10: // MFHI
        write_register(destination, high_register_);
        break;
    case 0x11: // MTHI
        high_register_ = registers_[source];
        break;
    case 0x12: // MFLO
        write_register(destination, low_register_);
        break;
    case 0x13: // MTLO
        low_register_ = registers_[source];
        break;
    case 0x16: // CLZ (Allegrex uses SPECIAL, rather than generic MIPS32 SPECIAL2).
        write_register(destination, static_cast<std::uint32_t>(std::countl_zero(registers_[source])));
        break;
    case 0x17: // CLO
        write_register(destination, static_cast<std::uint32_t>(std::countl_one(registers_[source])));
        break;
    case 0x18: // MULT
        write_hi_lo(signed_product(registers_[source], registers_[target]));
        break;
    case 0x19: // MULTU
        write_hi_lo(static_cast<std::uint64_t>(registers_[source]) * registers_[target]);
        break;
    case 0x1A: // DIV
    {
        const auto numerator = std::bit_cast<std::int32_t>(registers_[source]);
        const auto denominator = std::bit_cast<std::int32_t>(registers_[target]);
        // Handle Allegrex's special results before host division, which would be undefined.
        if (denominator == 0)
        {
            low_register_ = numerator < 0 ? 1U : kAllWordBits;
            high_register_ = registers_[source];
        }
        else if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1)
        {
            low_register_ = registers_[source];
            high_register_ = 0;
        }
        else
        {
            low_register_ = static_cast<std::uint32_t>(numerator / denominator);
            high_register_ = static_cast<std::uint32_t>(numerator % denominator);
        }
        break;
    }
    case 0x1B: // DIVU
        if (registers_[target] == 0)
        {
            // Hardware returns a 16-bit quotient for small numerators; see cpu_div.expected.
            low_register_ = registers_[source] <= kLowHalfwordMask ? kLowHalfwordMask : kAllWordBits;
            high_register_ = registers_[source];
        }
        else
        {
            low_register_ = registers_[source] / registers_[target];
            high_register_ = registers_[source] % registers_[target];
        }
        break;
    case 0x1C: // MADD (Allegrex SPECIAL encoding)
        write_hi_lo(hi_lo_value() + signed_product(registers_[source], registers_[target]));
        break;
    case 0x1D: // MADDU
        write_hi_lo(hi_lo_value() + static_cast<std::uint64_t>(registers_[source]) * registers_[target]);
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
    case 0x2C: // MAX: operands are signed.
        write_register(destination,
                       static_cast<std::uint32_t>(std::max(std::bit_cast<std::int32_t>(registers_[source]),
                                                           std::bit_cast<std::int32_t>(registers_[target]))));
        break;
    case 0x2D: // MIN
        write_register(destination,
                       static_cast<std::uint32_t>(std::min(std::bit_cast<std::int32_t>(registers_[source]),
                                                           std::bit_cast<std::int32_t>(registers_[target]))));
        break;
    case 0x2E: // MSUB
        write_hi_lo(hi_lo_value() - signed_product(registers_[source], registers_[target]));
        break;
    case 0x2F: // MSUBU
        write_hi_lo(hi_lo_value() - static_cast<std::uint64_t>(registers_[source]) * registers_[target]);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_special3(std::uint32_t instruction)
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
        write_register(target, (registers_[source] >> position) & mask);
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
        write_register(target, (registers_[target] & ~mask) | ((registers_[source] << position) & mask));
        break;
    }
    case 0x20: // BSHFL: shamt selects the byte/bit operation.
        switch (position)
        {
        case 0x02: // WSBH
            write_register(destination, ((registers_[target] & kOddByteMask) >> kByteBits) |
                                            ((registers_[target] & kEvenByteMask) << kByteBits));
            break;
        case 0x03: // WSBW
            write_register(destination, std::byteswap(registers_[target]));
            break;
        case 0x10: // SEB
            write_register(destination, static_cast<std::uint32_t>(
                                            std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(registers_[target]))));
            break;
        case 0x14: // BITREV
        {
            auto value = registers_[target];
            std::uint32_t result = 0;
            for (unsigned bit = 0; bit < kRegisterBits; ++bit)
            {
                result = (result << 1) | (value & 1U);
                value >>= 1;
            }
            write_register(destination, result);
            break;
        }
        case 0x18: // SEH
            write_register(destination, static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(
                                            static_cast<std::uint16_t>(registers_[target]))));
            break;
        default:
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
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
        write_register(kReturnAddressRegister, program_counter_.value_of() + kAfterDelaySlotOffset);
        break;
    case 0x11: // BGEZAL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(instruction);
        }
        write_register(kReturnAddressRegister, program_counter_.value_of() + kAfterDelaySlotOffset);
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
        write_register(kReturnAddressRegister, program_counter_.value_of() + kAfterDelaySlotOffset);
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
        write_register(kReturnAddressRegister, program_counter_.value_of() + kAfterDelaySlotOffset);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

GuestAddress Cpu::branch_address(std::uint32_t instruction) const
{
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto offset = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // Branch offsets count instructions relative to PC + 4. Unsigned arithmetic permits wrapping backwards.
    return GuestAddress{program_counter_.value_of() + kInstructionSize +
                        static_cast<std::uint32_t>(offset) * kInstructionSize};
}

void Cpu::skip_delay_slot(ControlFlow &flow) const
{
    // Untaken branch-likely instructions skip the slot, including its side effects and faults.
    flow.next_instruction = GuestAddress{program_counter_.value_of() + kAfterDelaySlotOffset};
    flow.following_instruction = GuestAddress{flow.next_instruction.value_of() + kInstructionSize};
}

GuestAddress Cpu::jump_address(std::uint32_t instruction) const
{
    // J/JAL retain the upper four bits of PC + 4 and supply the remaining word address.
    return GuestAddress{((program_counter_.value_of() + kInstructionSize) & kJumpRegionMask) |
                        ((instruction & kJumpTargetMask) * kInstructionSize)};
}

GuestAddress Cpu::word_address(std::uint32_t instruction) const
{
    const auto source = (instruction >> 21) & 0b11111U; // rs: bits 25-21
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // The offset is signed, but address arithmetic wraps modulo 2^32.
    const auto address = registers_[source] + static_cast<std::uint32_t>(signed_immediate);
    if ((address & kWordAlignmentMask) != 0)
    {
        throw std::invalid_argument("Word address must be aligned");
    }
    return GuestAddress{address};
}

std::uint64_t Cpu::hi_lo_value() const
{
    return (static_cast<std::uint64_t>(high_register_) << kRegisterBits) | low_register_;
}

void Cpu::write_hi_lo(std::uint64_t value)
{
    low_register_ = static_cast<std::uint32_t>(value);
    high_register_ = static_cast<std::uint32_t>(value >> kRegisterBits);
}

void Cpu::write_register(std::size_t index, std::uint32_t value)
{
    // Register zero is hardwired to zero; writes to it are discarded.
    if (index != kZeroRegister)
    {
        registers_[index] = value;
    }
}

} // namespace psp
