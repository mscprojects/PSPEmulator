#pragma once

#include "common/payload.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace psp::test
{

class PrxFixture
{
public:
    PrxFixture() : bytes(0x800)
    {
        word(0, 0x464C457F); // ELF magic
        bytes[4] = 1;        // ELF32
        bytes[5] = 1;        // little endian
        bytes[6] = 1;        // ELF version
        halfword(16, 0xFFA0);
        halfword(18, 8); // MIPS
        word(20, 1);
        word(24, 0);     // entry offset
        word(28, 52);    // program header table
        word(32, 0x600); // section header table
        halfword(40, 52);
        halfword(42, 32);
        halfword(44, 1);
        halfword(46, 40);
        halfword(48, 3);

        word(52, 1);             // PT_LOAD
        word(56, 0x100);         // file offset
        word(60, 0);             // virtual address
        word(64, 0x140);         // module-info file offset
        word(68, 0x180);         // file size
        word(72, 0x200);         // memory size includes BSS
        word(76, 5);             // PF_R | PF_X
        word(80, 4);             // alignment
        word(0x100, 0x24020007); // addiu $v0, $zero, 7

        halfword(0x140, 0x0002); // module attributes
        halfword(0x142, 0x0102); // module version
        name(0x144, "Fixture");
        word(0x160, 0x100); // GP
        word(0x16C, 0x80);  // imports begin
        word(0x170, 0x94);  // imports end

        word(0x180, 0xA0); // library name
        halfword(0x184, 0x0304);
        halfword(0x186, 0x4000);
        bytes[0x188] = 5;   // import record size in words
        halfword(0x18A, 1); // one function
        word(0x18C, 0xB0);  // NID table
        word(0x190, 0xC0);  // function stubs
        name(0x1A0, "TestLibrary");
        word(0x1B0, 0x12345678);
        word(0x1C0, 0x03E00008); // jr $ra; nop (left untouched by the loader)

        word(0x62C, 1);          // section 1: PROGBITS
        word(0x630, 2);          // SHF_ALLOC
        word(0x654, 0x700000A0); // section 2: PSP relocations
        word(0x660, 0x300);      // relocation file offset
        word(0x66C, 1);          // sh_info: target section
        word(0x674, 8);          // relocation record size
        relocation(0x60, 2);     // module GP
        relocation(0x6C, 2);     // import range
        relocation(0x70, 2);
        relocation(0x80, 2); // library name
        relocation(0x8C, 2); // NID table
        relocation(0x90, 2); // stub table
    }

    // Offsets and encoded values occupy fixed positions in this binary fixture writer.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void word(std::size_t file_offset, std::uint32_t value)
    {
        for (unsigned index = 0; index < 4; ++index)
        {
            bytes.at(file_offset + index) = static_cast<std::uint8_t>(value >> (index * 8));
        }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void halfword(std::size_t file_offset, std::uint16_t value)
    {
        bytes.at(file_offset) = static_cast<std::uint8_t>(value);
        bytes.at(file_offset + 1) = static_cast<std::uint8_t>(value >> 8);
    }

    void name(std::size_t file_offset, const std::string &value)
    {
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            bytes.at(file_offset + index) = static_cast<std::uint8_t>(value[index]);
        }
        bytes.at(file_offset + value.size()) = 0;
    }

    // r_offset and r_info are distinct fields of a fixed-format relocation record.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void relocation(std::uint32_t relative_address, std::uint32_t info)
    {
        word(0x300 + relocation_count * 8, relative_address);
        word(0x304 + relocation_count * 8, info);
        ++relocation_count;
        word(0x664, relocation_count * 8);
    }

    Payload bytes;
    std::uint32_t relocation_count{};
};

} // namespace psp::test
