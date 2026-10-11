#include "runtime/partition_memory.hpp"

#include <stdexcept>

namespace psp::detail
{

PartitionMemory::PartitionMemory(AddressArena &arena, std::uint32_t &next_id) : arena_(arena), next_id_(next_id)
{
}

std::uint32_t PartitionMemory::allocate(PartitionAllocation allocation)
{
    if (allocation.partition != 2 || allocation.type > 1)
    {
        throw std::runtime_error("Unsupported partition allocation");
    }
    const auto direction = allocation.type == 1 ? AllocationDirection::High : AllocationDirection::Low;
    const auto range = arena_.allocate(allocation.size, direction);
    const auto id = next_id_++;
    blocks_.emplace(id, range);
    return id;
}

void PartitionMemory::free(std::uint32_t id)
{
    arena_.free(blocks_.at(id));
    blocks_.erase(id);
}

std::uint32_t PartitionMemory::free_size() const
{
    return static_cast<std::uint32_t>(arena_.free_size());
}

std::uint32_t PartitionMemory::largest_free_size() const
{
    return static_cast<std::uint32_t>(arena_.largest_free_size());
}

GuestAddress PartitionMemory::address(std::uint32_t id) const
{
    return GuestAddress{static_cast<std::uint32_t>(blocks_.at(id).begin)};
}

} // namespace psp::detail
