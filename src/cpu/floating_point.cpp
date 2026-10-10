#include "cpu/floating_point.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

namespace psp
{

namespace
{

class HostFloatingPointEnvironment
{
public:
    explicit HostFloatingPointEnvironment(unsigned mode) : saved_errno_(errno)
    {
        if (std::fegetenv(&saved_) != 0)
        {
            throw std::runtime_error("Cannot save host floating-point environment");
        }
        constexpr std::array modes{FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
        // Default environment also disables inherited traps and host denormal flushing.
        if (std::fesetenv(FE_DFL_ENV) != 0 || std::fesetround(modes[mode]) != 0)
        {
            std::fesetenv(&saved_);
            errno = saved_errno_;
            throw std::runtime_error("Cannot set host floating-point environment");
        }
    }

    HostFloatingPointEnvironment(const HostFloatingPointEnvironment &) = delete;
    HostFloatingPointEnvironment &operator=(const HostFloatingPointEnvironment &) = delete;

    ~HostFloatingPointEnvironment()
    {
        std::fesetenv(&saved_);
        errno = saved_errno_;
    }

private:
    std::fenv_t saved_{};
    int saved_errno_;
};

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::digits == 24 && std::numeric_limits<float>::max_exponent == 128);
static_assert(FLT_EVAL_METHOD == 0, "Scalar FPU requires binary32 evaluation without excess precision");

constexpr std::uint32_t kInexact = 1U;
constexpr std::uint32_t kUnderflow = 2U;
constexpr std::uint32_t kOverflow = 4U;
constexpr std::uint32_t kDivideByZero = 8U;
constexpr std::uint32_t kInvalid = 16U;
constexpr std::uint32_t kMagnitude = 0x7FFFFFFFU;
constexpr std::uint32_t kInfinity = 0x7F800000U;
constexpr std::uint32_t kQuietNan = 0x7FC00000U;
constexpr std::uint32_t kQuietBit = 0x00400000U;

std::uint32_t host_exceptions()
{
    const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
    return ((flags & FE_INEXACT) != 0 ? kInexact : 0U) | ((flags & FE_UNDERFLOW) != 0 ? kUnderflow : 0U) |
           ((flags & FE_OVERFLOW) != 0 ? kOverflow : 0U) | ((flags & FE_DIVBYZERO) != 0 ? kDivideByZero : 0U) |
           ((flags & FE_INVALID) != 0 ? kInvalid : 0U);
}

// The binary32 payload and two-bit rounding selector have distinct roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
FloatingPointResult float_to_integer(std::uint32_t bits, unsigned mode)
{
    const auto magnitude = bits & kMagnitude;
    if (magnitude >= kInfinity)
    {
        // PSP NaNs saturate positive regardless of their sign or payload.
        return {magnitude > kInfinity || (bits >> 31) == 0 ? 0x7FFFFFFFU : 0x80000000U, kInvalid};
    }
    const HostFloatingPointEnvironment environment(mode);
    const double value = std::bit_cast<float>(bits);
    const double rounded = std::nearbyint(value);
    if (rounded >= 2'147'483'648.0)
    {
        return {0x7FFFFFFFU, kInvalid};
    }
    if (rounded < -2'147'483'648.0)
    {
        return {0x80000000U, kInvalid};
    }
    return {std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(rounded)), value != rounded ? kInexact : 0U};
}

} // namespace

// The decoder supplies two ordered operand words and a separate FCR31 word.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
FloatingPointResult evaluate_floating_point(FloatingPointOperation operation, std::uint32_t left, std::uint32_t right,
                                            std::uint32_t control)
{
    const unsigned mode = control & kFcrRoundingMode;
    switch (operation)
    {
    case FloatingPointOperation::Absolute:
        return {left & kMagnitude, 0};
    case FloatingPointOperation::Move:
        return {left, 0};
    case FloatingPointOperation::Negate:
        return {left ^ 0x80000000U, 0};
    case FloatingPointOperation::RoundToInteger:
        return float_to_integer(left, 0);
    case FloatingPointOperation::TruncateToInteger:
        return float_to_integer(left, 1);
    case FloatingPointOperation::CeilToInteger:
        return float_to_integer(left, 2);
    case FloatingPointOperation::FloorToInteger:
        return float_to_integer(left, 3);
    case FloatingPointOperation::ConvertToInteger:
        return float_to_integer(left, mode);
    case FloatingPointOperation::Add:
    case FloatingPointOperation::Subtract:
    case FloatingPointOperation::Multiply:
    case FloatingPointOperation::Divide:
    case FloatingPointOperation::SquareRoot:
    case FloatingPointOperation::ConvertFromInteger:
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex FPU operation");
    }

    if (operation != FloatingPointOperation::ConvertFromInteger)
    {
        if ((left & kMagnitude) > kInfinity)
        {
            return {left | kQuietBit, kInvalid};
        }
        if (operation != FloatingPointOperation::SquareRoot && (right & kMagnitude) > kInfinity)
        {
            return {right | kQuietBit, kInvalid};
        }
    }

    const HostFloatingPointEnvironment environment(mode);
    const float left_value = std::bit_cast<float>(left);
    const float right_value = std::bit_cast<float>(right);
    // The volatile store fixes binary32 precision before collecting exception flags.
    volatile float result = 0;
    switch (operation)
    {
    case FloatingPointOperation::Add:
        result = left_value + right_value;
        break;
    case FloatingPointOperation::Subtract:
        result = left_value - right_value;
        break;
    case FloatingPointOperation::Multiply:
        result = left_value * right_value;
        break;
    case FloatingPointOperation::Divide:
        result = left_value / right_value;
        break;
    case FloatingPointOperation::SquareRoot:
        result = std::sqrt(left_value);
        break;
    case FloatingPointOperation::ConvertFromInteger:
        result = static_cast<float>(std::bit_cast<std::int32_t>(left));
        break;
    default:
        throw std::runtime_error("Unsupported Allegrex FPU operation");
    }
    auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(result));
    auto exceptions = host_exceptions();
    if ((bits & kMagnitude) > kInfinity)
    {
        bits = kQuietNan; // Invalid operations create a positive canonical NaN on PSP.
    }
    if ((control & kFcrFlushToZero) != 0 && (bits & kMagnitude) != 0 && (bits & kMagnitude) < 0x00800000U)
    {
        bits &= 0x80000000U;
        exceptions |= kUnderflow | kInexact;
    }
    return {bits, exceptions};
}

FloatingPointComparison compare_floating_point(std::uint32_t left, std::uint32_t right, std::uint32_t predicate)
{
    const auto left_magnitude = left & kMagnitude;
    const auto right_magnitude = right & kMagnitude;
    // Quiet Allegrex compares accept both NaN encodings (including libc's 0x7FBFFFFF NAN).
    const bool unordered = left_magnitude > kInfinity || right_magnitude > kInfinity;
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
    const bool condition =
        ((predicate & 1U) != 0 && unordered) || ((predicate & 2U) != 0 && equal) || ((predicate & 4U) != 0 && less);
    return {condition, unordered && (predicate & 8U) != 0 ? kInvalid : 0U};
}

std::uint32_t floating_point_control_after(std::uint32_t control, std::uint32_t exceptions)
{
    if ((exceptions & (control >> kFcrEnableShift) & kFcrExceptions) != 0)
    {
        throw std::runtime_error("Enabled Allegrex FPU exception: guest exception entry is unsupported");
    }
    return (control & ~(kFcrExceptions << kFcrCauseShift)) | (exceptions << kFcrCauseShift) |
           (exceptions << kFcrFlagShift);
}

std::uint32_t floating_point_control_from_guest(std::uint32_t value)
{
    if ((value & kFcrUnimplementedCause) != 0)
    {
        throw std::runtime_error("FPU unimplemented-operation exception is unsupported");
    }
    if (((value >> kFcrCauseShift) & (value >> kFcrEnableShift) & kFcrExceptions) != 0)
    {
        throw std::runtime_error("Enabled Allegrex FPU exception in CTC1");
    }
    return value & kFcrWritable;
}

} // namespace psp
