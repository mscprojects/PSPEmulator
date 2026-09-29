#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp
{

class Memory
{
public:
    Memory(std::uint32_t base_address, std::size_t size);

    std::uint8_t read_u8(std::uint32_t address) const;
    std::uint16_t read_u16(std::uint32_t address) const;
    std::uint32_t read_u32(std::uint32_t address) const;

    void write_u8(std::uint32_t address, std::uint8_t value);
    void write_u16(std::uint32_t address, std::uint16_t value);
    void write_u32(std::uint32_t address, std::uint32_t value);

private:
    std::size_t checked_offset(std::uint32_t address, std::size_t width) const;
    std::uint32_t read_le(std::uint32_t address, std::size_t width) const;
    void write_le(std::uint32_t address, std::uint32_t value, std::size_t width);

    std::uint32_t base_address_;
    std::vector<std::uint8_t> bytes_;
};

} // namespace psp
