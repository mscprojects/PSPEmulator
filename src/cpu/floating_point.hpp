#pragma once

#include <cstdint>

namespace psp
{

// FCR31 control/status fields. The five exception bits (I, U, O, Z, V) appear three
// times: as sticky flags from bit 2, enables from bit 7, and causes from bit 12.
inline constexpr std::uint32_t kFcrRoundingMode = 0x3U;
inline constexpr unsigned kFcrFlagShift = 2;
inline constexpr unsigned kFcrEnableShift = 7;
inline constexpr unsigned kFcrCauseShift = 12;
inline constexpr std::uint32_t kFcrExceptions = 0x1FU;
inline constexpr std::uint32_t kFcrUnimplementedCause = 1U << 17;
inline constexpr std::uint32_t kFcrCondition = 1U << 23;
inline constexpr std::uint32_t kFcrFlushToZero = 1U << 24;
// Guest-writable bits: rounding, flags, enables, causes, condition, and flush-to-zero.
inline constexpr std::uint32_t kFcrWritable = kFcrRoundingMode | (kFcrExceptions << kFcrFlagShift) |
                                              (kFcrExceptions << kFcrEnableShift) | (kFcrExceptions << kFcrCauseShift) |
                                              kFcrCondition | kFcrFlushToZero;
static_assert(kFcrWritable == 0x0181FFFFU);

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

struct FloatingPointComparison
{
    bool condition;
    // Invalid (bit 4) when a signaling predicate sees a NaN; otherwise zero.
    std::uint32_t exceptions;
};

// Evaluates binary32 words with guest rounding and restores the host environment.
FloatingPointResult evaluate_floating_point(FloatingPointOperation operation, std::uint32_t left, std::uint32_t right,
                                            std::uint32_t control);
// C.cond.S on raw binary32 encodings, independent of host rounding and flush modes.
// predicate holds the low four function bits: unordered (bit 0), equal (bit 1),
// less (bit 2), and signaling (bit 3). Quiet compares accept both NaN encodings.
FloatingPointComparison compare_floating_point(std::uint32_t left, std::uint32_t right, std::uint32_t predicate);
// Clear previous causes, accumulate sticky flags, or throw for an enabled exception.
// Guest exception-vector entry remains unsupported; callers commit only after success.
std::uint32_t floating_point_control_after(std::uint32_t control, std::uint32_t exceptions);
// Validate a CTC1 write to FCR31 and return the stored value. Throws when the write sets
// the unimplemented-operation cause or any cause whose exception is enabled.
std::uint32_t floating_point_control_from_guest(std::uint32_t value);

} // namespace psp
