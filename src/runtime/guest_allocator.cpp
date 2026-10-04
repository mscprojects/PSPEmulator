#include "runtime/guest_allocator.hpp"

#include <algorithm>
#include <stdexcept>

namespace psp::detail
{

GuestAllocator::GuestAllocator(GuestAddress load_address, std::size_t memory_size, std::span<const PrxSegment> segments)
    : heap_(load_address.value_of()),
      stack_top_((static_cast<std::uint64_t>(load_address.value_of()) + memory_size) & ~std::uint64_t{255})
{
    for (const auto &segment : segments)
    {
        if (segment.type == 1)
        {
            heap_ = std::max(heap_, static_cast<std::uint64_t>(load_address.value_of()) + segment.virtual_address +
                                        segment.memory_size);
        }
    }
    heap_ = (heap_ + 255) & ~std::uint64_t{255};
}

GuestAddress GuestAllocator::allocate(std::uint32_t size, bool high)
{
    const auto aligned_size = (static_cast<std::uint64_t>(size) + 255) & ~std::uint64_t{255};
    if (size == 0 || heap_ > stack_top_ || aligned_size > stack_top_ - heap_)
    {
        throw std::runtime_error("Guest memory exhausted");
    }
    if (high)
    {
        stack_top_ -= aligned_size;
        return GuestAddress{static_cast<std::uint32_t>(stack_top_)};
    }
    const auto address = GuestAddress{static_cast<std::uint32_t>(heap_)};
    heap_ += aligned_size;
    return address;
}

std::uint32_t GuestAllocator::free_size() const
{
    return static_cast<std::uint32_t>(stack_top_ - heap_);
}

std::uint32_t GuestAllocator::allocate_partition(std::uint32_t id, PartitionAllocation allocation)
{
    if (allocation.partition != 2 || allocation.type > 1)
    {
        throw std::runtime_error("Unsupported partition allocation");
    }
    blocks_.emplace(id, allocate(allocation.size, allocation.type == 1));
    return id;
}

GuestAddress GuestAllocator::block_address(std::uint32_t id) const
{
    return blocks_.at(id);
}

} // namespace psp::detail
