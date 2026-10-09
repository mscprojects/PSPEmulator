#pragma once

#include <cstdint>

namespace psp
{

// Host input uses PSP button bits and unsigned analog coordinates (128 is neutral).
struct ControllerState
{
    std::uint32_t buttons{};
    std::uint8_t left_x{128};
    std::uint8_t left_y{128};
    std::uint8_t right_x{128};
    std::uint8_t right_y{128};

    bool operator==(const ControllerState &) const = default;
};

} // namespace psp
