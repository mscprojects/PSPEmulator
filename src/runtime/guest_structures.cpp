#include "runtime/guest_structures.hpp"

#include <bit>
#include <span>

namespace psp::detail
{

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);

GuestWord::GuestWord(std::uint32_t value)
    : bytes(std::bit_cast<std::array<std::uint8_t, 4>>(
          std::endian::native == std::endian::little ? value : std::byteswap(value)))
{
}

std::uint32_t GuestWord::value() const
{
    const auto native = std::bit_cast<std::uint32_t>(bytes);
    return std::endian::native == std::endian::little ? native : std::byteswap(native);
}

void write_thread_info(Memory &memory, GuestAddress address, const GuestThreadInfo &info)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(info)>>(info));
}

GuestMutexWorkArea read_mutex_work_area(const Memory &memory, GuestAddress address)
{
    GuestMutexWorkArea work_area;
    memory.read_into(address, std::as_writable_bytes(std::span{&work_area, 1}));
    return work_area;
}

void write_mutex_work_area(Memory &memory, GuestAddress address, const GuestMutexWorkArea &work_area)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(work_area)>>(work_area));
}

void write_controller_data(Memory &memory, GuestAddress address, const GuestControllerData &data)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(data)>>(data));
}

} // namespace psp::detail
