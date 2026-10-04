#pragma once

#include "memory/memory.hpp"

#include <string>

namespace psp::detail
{

struct DeviceControl
{
    GuestAddress device;
    std::uint32_t command;
    GuestAddress input;
    std::uint32_t input_size;
    GuestAddress output;
    std::uint32_t output_size;
};

// Captures guest-produced console bytes and implements the autotest emulator
// device protocol. No host filesystem or display is exposed.
class GuestIo
{
public:
    explicit GuestIo(Memory &memory);
    std::uint32_t write(std::uint32_t descriptor, GuestAddress buffer, std::uint32_t size);
    void device_control(const DeviceControl &request);
    std::string take_output();

private:
    Memory &memory_;
    std::string output_;
};

} // namespace psp::detail
