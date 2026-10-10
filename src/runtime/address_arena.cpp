#include "runtime/address_arena.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace psp::detail
{

namespace
{

constexpr std::uint64_t kAlignment = 256;

} // namespace

AddressArena::AddressArena(std::uint64_t begin, std::uint64_t end)
{
    if (begin > end || end > (std::uint64_t{1} << 32))
    {
        throw std::invalid_argument("Arena must be an ordered range within the 32-bit address space");
    }
    begin = (begin + kAlignment - 1) & ~(kAlignment - 1);
    end &= ~(kAlignment - 1);
    if (begin < end)
    {
        free_ranges_.push_back({begin, end});
    }
}

AddressRange AddressArena::allocate(std::uint32_t size, AllocationDirection direction)
{
    if (size == 0)
    {
        throw std::runtime_error("Guest allocation size must be positive");
    }
    const auto aligned_size = (static_cast<std::uint64_t>(size) + kAlignment - 1) & ~(kAlignment - 1);
    for (std::size_t offset = 0; offset < free_ranges_.size(); ++offset)
    {
        const auto index = direction == AllocationDirection::Low ? offset : free_ranges_.size() - 1 - offset;
        auto &range = free_ranges_[index];
        if (aligned_size > range.end - range.begin)
        {
            continue;
        }
        AddressRange allocation{};
        if (direction == AllocationDirection::Low)
        {
            allocation = {range.begin, range.begin + aligned_size};
            range.begin = allocation.end;
        }
        else
        {
            allocation = {range.end - aligned_size, range.end};
            range.end = allocation.begin;
        }
        if (range.begin == range.end)
        {
            free_ranges_.erase(free_ranges_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        return allocation;
    }
    throw std::runtime_error("Guest memory exhausted");
}

void AddressArena::free(AddressRange range)
{
    auto position =
        std::lower_bound(free_ranges_.begin(), free_ranges_.end(), range.begin,
                         [](const AddressRange &free, std::uint64_t address) { return free.begin < address; });
    position = free_ranges_.insert(position, range);
    if (position != free_ranges_.begin() && std::prev(position)->end == position->begin)
    {
        std::prev(position)->end = position->end;
        position = std::prev(free_ranges_.erase(position));
    }
    if (std::next(position) != free_ranges_.end() && position->end == std::next(position)->begin)
    {
        position->end = std::next(position)->end;
        free_ranges_.erase(std::next(position));
    }
}

std::uint64_t AddressArena::free_size() const
{
    std::uint64_t total = 0;
    for (const auto &range : free_ranges_)
    {
        total += range.end - range.begin;
    }
    return total;
}

std::uint64_t AddressArena::largest_free_size() const
{
    std::uint64_t largest = 0;
    for (const auto &range : free_ranges_)
    {
        largest = std::max(largest, range.end - range.begin);
    }
    return largest;
}

} // namespace psp::detail
