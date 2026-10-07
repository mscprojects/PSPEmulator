#pragma once

#include <cstdint>

namespace psp
{

// Values match the COP1 function field; ConvertFromInteger uses the word format.
enum class FloatingPointOperation : std::uint8_t
{
    Add = 0x00,
    Subtract = 0x01,
    Multiply = 0x02,
    Divide = 0x03,
    SquareRoot = 0x04,
    Absolute = 0x05,
    Move = 0x06,
    Negate = 0x07,
    RoundToInteger = 0x0C,
    TruncateToInteger = 0x0D,
    CeilToInteger = 0x0E,
    FloorToInteger = 0x0F,
    ConvertFromInteger = 0x20,
    ConvertToInteger = 0x24,
};

struct FloatingPointResult
{
    std::uint32_t value;
    // I, U, O, Z, V in bits 0-4, before shifting into FCR31.
    std::uint32_t exceptions;
};

// Evaluates binary32 words with guest rounding and restores the host environment.
FloatingPointResult evaluate_floating_point(FloatingPointOperation operation, std::uint32_t left, std::uint32_t right,
                                            std::uint32_t control);
// Clear previous causes, accumulate sticky flags, or throw for an enabled exception.
// Guest exception-vector entry remains unsupported; callers commit only after success.
std::uint32_t floating_point_control_after(std::uint32_t control, std::uint32_t exceptions);

} // namespace psp
