#include "runtime/guest_io.hpp"

#include <stdexcept>
#include <utility>

namespace psp::detail
{

GuestIo::GuestIo(Memory &memory) : memory_(memory)
{
}

std::uint32_t GuestIo::write(std::uint32_t descriptor, GuestAddress buffer, std::uint32_t size)
{
    if (descriptor != 1 && descriptor != 2)
    {
        throw std::runtime_error("Only stdout and stderr writes are supported");
    }
    const auto bytes = memory_.read_bytes(buffer, size);
    output_.append(bytes.begin(), bytes.end());
    return size;
}

void GuestIo::device_control(const DeviceControl &request)
{
    const auto device = memory_.read_c_string(request.device, 4096);
    if (device != "emulator:" && device != "kemulator:")
    {
        throw std::runtime_error("Unsupported devctl device " + device);
    }
    switch (request.command)
    {
    case 1: // Headless runtime has no display.
        if (request.output_size < 4)
        {
            throw std::runtime_error("Display query buffer is too small");
        }
        memory_.write_u32(request.output, 0);
        return;
    case 2: // Guest libc produces these bytes; the host performs no formatting.
    {
        const auto bytes = memory_.read_bytes(request.input, request.input_size);
        output_.append(bytes.begin(), bytes.end());
        return;
    }
    case 3: // Emulator probe.
        return;
    default:
        throw std::runtime_error("Unsupported emulator devctl command");
    }
}

std::string GuestIo::take_output()
{
    return std::move(output_);
}

} // namespace psp::detail
