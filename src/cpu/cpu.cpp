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
constexpr std::uint32_t kFloatingPointCondition = 1U << 23;
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

std::optional<Syscall> Cpu::step(CpuState &state)
{
    if ((state.program_counter.value_of() & kWordAlignmentMask) != 0)
    {
        throw std::invalid_argument("Instruction address must be word aligned");
    }
    const auto instruction = memory_.read_u32(state.program_counter);
    // A caller may edit a saved state directly; $zero is always zero for operands.
    state.registers[0] = 0;
    ControlFlow flow{state.next_program_counter, GuestAddress{state.next_program_counter.value_of() + kInstructionSize},
                     (state.floating_point_control & kFloatingPointCondition) != 0};
    std::optional<Syscall> syscall;
    if ((instruction & 0xFC00003FU) == 0x0000000CU)
    {
        state.load_linked = false;
        syscall = Syscall{(instruction >> 6) & 0xFFFFFU, state.program_counter};
    }
    else
    {
        execute(state, instruction, flow);
    }
    state.floating_point_branch_condition = flow.floating_point_branch_condition;
    state.program_counter = flow.next_instruction;
    state.next_program_counter = flow.following_instruction;
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
        write_register(state, kReturnAddressRegister, state.program_counter.value_of() + kAfterDelaySlotOffset);
        break;
    case 0x04: // BEQ
        if (state.registers[source] == state.registers[target])
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x05: // BNE
        if (state.registers[source] != state.registers[target])
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x06: // BLEZ
        if (std::bit_cast<std::int32_t>(state.registers[source]) <= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x07: // BGTZ
        if (std::bit_cast<std::int32_t>(state.registers[source]) > 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        break;
    case 0x09: // ADDIU
        // The immediate is sign-extended; unsigned addition wraps without an overflow trap.
        write_register(state, target, state.registers[source] + static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0A: // SLTI
        write_register(state, target, std::bit_cast<std::int32_t>(state.registers[source]) < signed_immediate);
        break;
    case 0x0B: // SLTIU
        // Even for an unsigned comparison, the immediate is sign-extended first.
        write_register(state, target, state.registers[source] < static_cast<std::uint32_t>(signed_immediate));
        break;
    case 0x0C: // ANDI
        // Logical immediates (ANDI, ORI, XORI) are zero-extended instead.
        write_register(state, target, state.registers[source] & immediate);
        break;
    case 0x0D: // ORI
        write_register(state, target, state.registers[source] | immediate);
        break;
    case 0x0E: // XORI
        write_register(state, target, state.registers[source] ^ immediate);
        break;
    case 0x0F: // LUI
        write_register(state, target, static_cast<std::uint32_t>(immediate) << 16);
        break;
    case 0x11: // COP1: scalar floating-point operations.
        execute_cop1(state, instruction, flow);
        break;
    case 0x14: // BEQL
        if (state.registers[source] == state.registers[target])
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x15: // BNEL
        if (state.registers[source] != state.registers[target])
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x16: // BLEZL
        if (std::bit_cast<std::int32_t>(state.registers[source]) <= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else
        {
            skip_delay_slot(state, flow);
        }
        break;
    case 0x17: // BGTZL
        if (std::bit_cast<std::int32_t>(state.registers[source]) > 0)
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
        write_register(state, target,
                       static_cast<std::uint32_t>(std::bit_cast<std::int8_t>(
                           memory_.read_u8(data_address(state, instruction, DataAlignment::Byte)))));
        break;
    case 0x21: // LH
        write_register(state, target,
                       static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(
                           memory_.read_u16(data_address(state, instruction, DataAlignment::Halfword)))));
        break;
    case 0x22: // LWL: merge memory bytes into the left (most significant) register bytes.
    {
        const auto address = data_address(state, instruction, DataAlignment::Byte).value_of();
        const auto shift = (3U - (address & 3U)) * 8;
        const auto word = memory_.read_u32(GuestAddress{address & ~3U});
        const auto mask = 0xFFFFFFFFU << shift;
        write_register(state, target, (state.registers[target] & ~mask) | (word << shift));
        break;
    }
    case 0x23: // LW
        write_register(state, target, memory_.read_u32(data_address(state, instruction, DataAlignment::Word)));
        break;
    case 0x24: // LBU
        write_register(state, target, memory_.read_u8(data_address(state, instruction, DataAlignment::Byte)));
        break;
    case 0x25: // LHU
        write_register(state, target, memory_.read_u16(data_address(state, instruction, DataAlignment::Halfword)));
        break;
    case 0x26: // LWR: merge memory bytes into the right (least significant) register bytes.
    {
        const auto address = data_address(state, instruction, DataAlignment::Byte).value_of();
        const auto shift = (address & 3U) * 8;
        const auto word = memory_.read_u32(GuestAddress{address & ~3U});
        const auto mask = 0xFFFFFFFFU >> shift;
        write_register(state, target, (state.registers[target] & ~mask) | (word >> shift));
        break;
    }
    case 0x28: // SB
        memory_.write_u8(data_address(state, instruction, DataAlignment::Byte),
                         static_cast<std::uint8_t>(state.registers[target]));
        break;
    case 0x29: // SH
        memory_.write_u16(data_address(state, instruction, DataAlignment::Halfword),
                          static_cast<std::uint16_t>(state.registers[target]));
        break;
    case 0x2A: // SWL: store the register's left bytes, preserving the rest of the memory word.
    {
        const auto address = data_address(state, instruction, DataAlignment::Byte).value_of();
        const auto aligned = GuestAddress{address & ~3U};
        const auto shift = (3U - (address & 3U)) * 8;
        const auto word = memory_.read_u32(aligned);
        const auto mask = 0xFFFFFFFFU >> shift;
        memory_.write_u32(aligned, (word & ~mask) | (state.registers[target] >> shift));
        break;
    }
    case 0x2B: // SW
        memory_.write_u32(data_address(state, instruction, DataAlignment::Word), state.registers[target]);
        break;
    case 0x2E: // SWR: store the register's right bytes, preserving the rest of the memory word.
    {
        const auto address = data_address(state, instruction, DataAlignment::Byte).value_of();
        const auto aligned = GuestAddress{address & ~3U};
        const auto shift = (address & 3U) * 8;
        const auto word = memory_.read_u32(aligned);
        const auto mask = 0xFFFFFFFFU << shift;
        memory_.write_u32(aligned, (word & ~mask) | (state.registers[target] << shift));
        break;
    }
    case 0x30: // LL: load an aligned word and set the Allegrex link bit.
        write_register(state, target, memory_.read_u32(data_address(state, instruction, DataAlignment::Word)));
        state.load_linked = true;
        break;
    case 0x31: // LWC1: transfer bits without numeric conversion.
        state.floating_point_registers[target] =
            memory_.read_u32(data_address(state, instruction, DataAlignment::Word));
        break;
    case 0x39: // SWC1
        memory_.write_u32(data_address(state, instruction, DataAlignment::Word),
                          state.floating_point_registers[target]);
        break;
    case 0x38: // SC: store when linked, then report success in rt. Allegrex keeps the bit set.
    {
        const auto address = data_address(state, instruction, DataAlignment::Word);
        if (state.load_linked)
        {
            memory_.write_u32(address, state.registers[target]);
        }
        write_register(state, target, static_cast<std::uint32_t>(state.load_linked));
        break;
    }
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
        write_register(state, destination, state.registers[target] << shift);
        break;
    case 0x02: // SRL / ROTR: rs is an operation selector here.
        if (source == 0)
        {
            write_register(state, destination, state.registers[target] >> shift);
        }
        else if (source == 1)
        {
            write_register(state, destination, std::rotr(state.registers[target], static_cast<int>(shift)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x03: // SRA
        write_register(state, destination,
                       std::bit_cast<std::uint32_t>(std::bit_cast<std::int32_t>(state.registers[target]) >> shift));
        break;
    case 0x04: // SLLV: variable shifts use only the low five count bits.
        write_register(state, destination, state.registers[target] << (state.registers[source] & kShiftCountMask));
        break;
    case 0x06: // SRLV / ROTRV: shamt selects shift or rotate.
        if (shift == 0)
        {
            write_register(state, destination, state.registers[target] >> (state.registers[source] & kShiftCountMask));
        }
        else if (shift == 1)
        {
            write_register(
                state, destination,
                std::rotr(state.registers[target], static_cast<int>(state.registers[source] & kShiftCountMask)));
        }
        else
        {
            throw std::runtime_error("Unsupported Allegrex instruction");
        }
        break;
    case 0x07: // SRAV
        write_register(state, destination,
                       static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(state.registers[target]) >>
                                                  (state.registers[source] & kShiftCountMask)));
        break;
    case 0x08: // JR
        flow.following_instruction = GuestAddress{state.registers[source]};
        break;
    case 0x09: // JALR
        // Capture rs before writing rd, which may be the same register.
        flow.following_instruction = GuestAddress{state.registers[source]};
        write_register(state, destination, state.program_counter.value_of() + kAfterDelaySlotOffset);
        break;
    case 0x0A: // MOVZ
        if (state.registers[target] == 0)
        {
            write_register(state, destination, state.registers[source]);
        }
        break;
    case 0x0B: // MOVN
        if (state.registers[target] != 0)
        {
            write_register(state, destination, state.registers[source]);
        }
        break;
    case 0x10: // MFHI
        write_register(state, destination, state.high_register);
        break;
    case 0x11: // MTHI
        state.high_register = state.registers[source];
        break;
    case 0x12: // MFLO
        write_register(state, destination, state.low_register);
        break;
    case 0x13: // MTLO
        state.low_register = state.registers[source];
        break;
    case 0x16: // CLZ (Allegrex uses SPECIAL, rather than generic MIPS32 SPECIAL2).
        write_register(state, destination, static_cast<std::uint32_t>(std::countl_zero(state.registers[source])));
        break;
    case 0x17: // CLO
        write_register(state, destination, static_cast<std::uint32_t>(std::countl_one(state.registers[source])));
        break;
    case 0x18: // MULT
        write_hi_lo(state, signed_product(state.registers[source], state.registers[target]));
        break;
    case 0x19: // MULTU
        write_hi_lo(state, static_cast<std::uint64_t>(state.registers[source]) * state.registers[target]);
        break;
    case 0x1A: // DIV
    {
        const auto numerator = std::bit_cast<std::int32_t>(state.registers[source]);
        const auto denominator = std::bit_cast<std::int32_t>(state.registers[target]);
        // Handle Allegrex's special results before host division, which would be undefined.
        if (denominator == 0)
        {
            state.low_register = numerator < 0 ? 1U : 0xFFFFFFFFU;
            state.high_register = state.registers[source];
        }
        else if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1)
        {
            state.low_register = state.registers[source];
            state.high_register = 0;
        }
        else
        {
            state.low_register = static_cast<std::uint32_t>(numerator / denominator);
            state.high_register = static_cast<std::uint32_t>(numerator % denominator);
        }
        break;
    }
    case 0x1B: // DIVU
        if (state.registers[target] == 0)
        {
            // Hardware returns a 16-bit quotient for small numerators; see cpu_div.expected.
            state.low_register = state.registers[source] <= kLowHalfwordMask ? kLowHalfwordMask : 0xFFFFFFFFU;
            state.high_register = state.registers[source];
        }
        else
        {
            state.low_register = state.registers[source] / state.registers[target];
            state.high_register = state.registers[source] % state.registers[target];
        }
        break;
    case 0x1C: // MADD (Allegrex SPECIAL encoding)
        write_hi_lo(state, hi_lo_value(state) + signed_product(state.registers[source], state.registers[target]));
        break;
    case 0x1D: // MADDU
        write_hi_lo(state,
                    hi_lo_value(state) + static_cast<std::uint64_t>(state.registers[source]) * state.registers[target]);
        break;
    case 0x21: // ADDU
        write_register(state, destination, state.registers[source] + state.registers[target]);
        break;
    case 0x23: // SUBU
        write_register(state, destination, state.registers[source] - state.registers[target]);
        break;
    case 0x24: // AND
        write_register(state, destination, state.registers[source] & state.registers[target]);
        break;
    case 0x25: // OR
        write_register(state, destination, state.registers[source] | state.registers[target]);
        break;
    case 0x26: // XOR
        write_register(state, destination, state.registers[source] ^ state.registers[target]);
        break;
    case 0x27: // NOR
        write_register(state, destination, ~(state.registers[source] | state.registers[target]));
        break;
    case 0x2A: // SLT
        write_register(state, destination,
                       std::bit_cast<std::int32_t>(state.registers[source]) <
                           std::bit_cast<std::int32_t>(state.registers[target]));
        break;
    case 0x2B: // SLTU
        write_register(state, destination, state.registers[source] < state.registers[target]);
        break;
    case 0x2C: // MAX: operands are signed.
        write_register(state, destination,
                       static_cast<std::uint32_t>(std::max(std::bit_cast<std::int32_t>(state.registers[source]),
                                                           std::bit_cast<std::int32_t>(state.registers[target]))));
        break;
    case 0x2D: // MIN
        write_register(state, destination,
                       static_cast<std::uint32_t>(std::min(std::bit_cast<std::int32_t>(state.registers[source]),
                                                           std::bit_cast<std::int32_t>(state.registers[target]))));
        break;
    case 0x2E: // MSUB
        write_hi_lo(state, hi_lo_value(state) - signed_product(state.registers[source], state.registers[target]));
        break;
    case 0x2F: // MSUBU
        write_hi_lo(state,
                    hi_lo_value(state) - static_cast<std::uint64_t>(state.registers[source]) * state.registers[target]);
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
        write_register(state, target, (state.registers[source] >> position) & mask);
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
        write_register(state, target,
                       (state.registers[target] & ~mask) | ((state.registers[source] << position) & mask));
        break;
    }
    case 0x20: // BSHFL: shamt selects the byte/bit operation.
        switch (position)
        {
        case 0x02: // WSBH
            write_register(state, destination,
                           ((state.registers[target] & kOddByteMask) >> 8) |
                               ((state.registers[target] & kEvenByteMask) << 8));
            break;
        case 0x03: // WSBW
            write_register(state, destination, std::byteswap(state.registers[target]));
            break;
        case 0x10: // SEB
            write_register(state, destination,
                           static_cast<std::uint32_t>(
                               std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(state.registers[target]))));
            break;
        case 0x14: // BITREV
        {
            auto value = state.registers[target];
            std::uint32_t result = 0;
            for (unsigned bit = 0; bit < kRegisterBits; ++bit)
            {
                result = (result << 1) | (value & 1U);
                value >>= 1;
            }
            write_register(state, destination, result);
            break;
        }
        case 0x18: // SEH
            write_register(state, destination,
                           static_cast<std::uint32_t>(
                               std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(state.registers[target]))));
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
    const auto value = std::bit_cast<std::int32_t>(state.registers[source]);

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
        write_register(state, kReturnAddressRegister, state.program_counter.value_of() + kAfterDelaySlotOffset);
        break;
    case 0x11: // BGEZAL
        if (value >= 0)
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        write_register(state, kReturnAddressRegister, state.program_counter.value_of() + kAfterDelaySlotOffset);
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
        write_register(state, kReturnAddressRegister, state.program_counter.value_of() + kAfterDelaySlotOffset);
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
        write_register(state, kReturnAddressRegister, state.program_counter.value_of() + kAfterDelaySlotOffset);
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex instruction");
    }
}

void Cpu::execute_cop1(CpuState &state, std::uint32_t instruction, ControlFlow &flow)
{
    const auto operation = (instruction >> 21) & 31U;
    const auto target = (instruction >> 16) & 31U; // rt or ft
    const auto source = (instruction >> 11) & 31U; // fs or control register
    switch (operation)
    {
    case 0x00: // MFC1
        write_register(state, target, state.floating_point_registers[source]);
        return;
    case 0x02: // CFC1: FCR0 is read-only; other control registers read zero.
    {
        std::uint32_t value = 0;
        if (source == 31)
        {
            value = state.floating_point_control;
        }
        else if (source == 0)
        {
            value = 0x3351U;
        }
        write_register(state, target, value);
        return;
    }
    case 0x04: // MTC1 (including writable $f0)
        state.floating_point_registers[source] = state.registers[target];
        return;
    case 0x06: // CTC1
        if (source == 31)
        {
            if ((state.registers[target] & 0x00020000U) != 0)
            {
                throw std::runtime_error("FPU unimplemented-operation exception is unsupported");
            }
            state.floating_point_control = state.registers[target] & 0x0181FFFFU;
            // Unlike a comparison, CTC1 is immediately visible to a following branch.
            flow.floating_point_branch_condition = (state.floating_point_control & kFloatingPointCondition) != 0;
        }
        return;
    case 0x08: // BC1F, BC1T, BC1FL, BC1TL
        if (target > 3)
        {
            throw std::runtime_error("Unsupported Allegrex FPU branch selector");
        }
        if (state.floating_point_branch_condition == ((target & 1U) != 0))
        {
            flow.following_instruction = branch_address(state, instruction);
        }
        else if ((target & 2U) != 0)
        {
            skip_delay_slot(state, flow);
        }
        return;
    case 0x10:                                    // Single precision transfers and comparisons.
        if ((instruction & 0x001F003FU) == 0x06U) // MOV.S: ft must be zero.
        {
            state.floating_point_registers[(instruction >> 6) & 31U] = state.floating_point_registers[source];
            return;
        }
        if ((instruction & 0x7F0U) == 0x30U)
        {
            const auto left = state.floating_point_registers[source];
            const auto right = state.floating_point_registers[target];
            const auto left_magnitude = left & 0x7FFFFFFFU;
            const auto right_magnitude = right & 0x7FFFFFFFU;
            const bool left_nan = left_magnitude > 0x7F800000U;
            const bool right_nan = right_magnitude > 0x7F800000U;
            const bool unordered = left_nan || right_nan;
            const auto predicate = instruction & 15U;
            // Exception flags and guest exception entry belong to the arithmetic milestone.
            if ((unordered && (predicate & 8U) != 0) || (left_nan && (left & 0x00400000U) == 0) ||
                (right_nan && (right & 0x00400000U) == 0))
            {
                throw std::runtime_error("FPU invalid-operation exception is unsupported");
            }
            // Compare binary32 encodings directly, independent of host rounding/flush modes.
            const bool equal = !unordered && (left == right || (left_magnitude == 0 && right_magnitude == 0));
            bool less = false;
            if (!unordered && !equal)
            {
                if (((left ^ right) & 0x80000000U) != 0)
                {
                    less = (left >> 31) != 0;
                }
                else if ((left >> 31) != 0)
                {
                    less = left > right;
                }
                else
                {
                    less = left < right;
                }
            }
            const bool condition = ((predicate & 1U) != 0 && unordered) || ((predicate & 2U) != 0 && equal) ||
                                   ((predicate & 4U) != 0 && less);
            state.floating_point_control =
                (state.floating_point_control & ~kFloatingPointCondition) | (condition ? kFloatingPointCondition : 0U);
            return;
        }
        break;
    default:
        break;
    }
    throw std::runtime_error("Unsupported Allegrex FPU instruction");
}

GuestAddress Cpu::branch_address(const CpuState &state, std::uint32_t instruction) const
{
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto offset = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // Branch offsets count instructions relative to PC + 4. Unsigned arithmetic permits wrapping backwards.
    return GuestAddress{state.program_counter.value_of() + kInstructionSize +
                        static_cast<std::uint32_t>(offset) * kInstructionSize};
}

void Cpu::skip_delay_slot(const CpuState &state, ControlFlow &flow) const
{
    // Untaken branch-likely instructions skip the slot, including its side effects and faults.
    flow.next_instruction = GuestAddress{state.program_counter.value_of() + kAfterDelaySlotOffset};
    flow.following_instruction = GuestAddress{flow.next_instruction.value_of() + kInstructionSize};
}

GuestAddress Cpu::jump_address(const CpuState &state, std::uint32_t instruction) const
{
    // J/JAL retain the upper four bits of PC + 4 and supply the remaining word address.
    return GuestAddress{((state.program_counter.value_of() + kInstructionSize) & kJumpRegionMask) |
                        ((instruction & kJumpTargetMask) * kInstructionSize)};
}

GuestAddress Cpu::data_address(const CpuState &state, std::uint32_t instruction, DataAlignment alignment) const
{
    const auto source = (instruction >> 21) & 0b11111U; // rs: bits 25-21
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const auto signed_immediate = static_cast<std::int32_t>(std::bit_cast<std::int16_t>(immediate));
    // The offset is signed, but address arithmetic wraps modulo 2^32.
    const auto address = state.registers[source] + static_cast<std::uint32_t>(signed_immediate);
    if (address % static_cast<std::uint32_t>(alignment) != 0)
    {
        throw std::invalid_argument("Data address must be aligned");
    }
    return GuestAddress{address};
}

std::uint64_t Cpu::hi_lo_value(const CpuState &state) const
{
    return (static_cast<std::uint64_t>(state.high_register) << kRegisterBits) | state.low_register;
}

void Cpu::write_hi_lo(CpuState &state, std::uint64_t value)
{
    state.low_register = static_cast<std::uint32_t>(value);
    state.high_register = static_cast<std::uint32_t>(value >> kRegisterBits);
}

void Cpu::write_register(CpuState &state, std::size_t index, std::uint32_t value)
{
    if (index != 0)
    {
        state.registers[index] = value;
    }
}

} // namespace psp
