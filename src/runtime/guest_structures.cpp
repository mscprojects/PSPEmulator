#include "runtime/guest_structures.hpp"

#include <algorithm>
#include <bit>

namespace psp::detail
{

GuestWord::GuestWord(std::uint32_t value)
{
    for (unsigned index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint32_t GuestWord::value() const
{
    std::uint32_t result = 0;
    for (unsigned index = 0; index < bytes.size(); ++index)
    {
        result |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return result;
}

void write_thread_info(Memory &memory, GuestAddress address, const GuestThreadInfo &info)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(info)>>(info));
}

GuestMutexWorkArea read_mutex_work_area(const Memory &memory, GuestAddress address)
{
    const auto payload = memory.read_bytes(address, sizeof(GuestMutexWorkArea));
    std::array<std::uint8_t, sizeof(GuestMutexWorkArea)> bytes{};
    std::ranges::copy(payload, bytes.begin());
    return std::bit_cast<GuestMutexWorkArea>(bytes);
}

void write_mutex_work_area(Memory &memory, GuestAddress address, const GuestMutexWorkArea &work_area)
{
    memory.write_bytes(address, std::bit_cast<std::array<std::uint8_t, sizeof(work_area)>>(work_area));
}

} // namespace psp::detail
