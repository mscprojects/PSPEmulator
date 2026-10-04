#include "runtime/guest_structures.hpp"

#include <bit>
#include <cstring>

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
    const auto payload = memory.read_bytes(address, sizeof(GuestMutexWorkArea));
    GuestMutexWorkArea work_area;
    std::memcpy(&work_area, payload.data(), sizeof(work_area));
    return work_area;
}

void write_mutex_work_area(Memory &memory, GuestAddress address, const GuestMutexWorkArea &work_area)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(work_area)>>(work_area));
}

} // namespace psp::detail
