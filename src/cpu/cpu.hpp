#pragma once

#include "memory/address.hpp"
#include "memory/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psp
{

class Cpu
{
public:
    Cpu(Memory &memory, GuestAddress entry_point);

    void step();

    GuestAddress program_counter() const;
    std::uint32_t register_value(std::size_t index) const;

private:
    void execute(std::uint32_t instruction);
    void execute_special(std::uint32_t instruction);
    void write_register(std::size_t index, std::uint32_t value);

    Memory &memory_;
    GuestAddress program_counter_;
    std::array<std::uint32_t, 32> registers_{};
};

} // namespace psp
