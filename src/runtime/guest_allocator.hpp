#pragma once

#include "loader/prx_reader.hpp"
#include "memory/address.hpp"

#include <map>
#include <span>

namespace psp::detail
{

struct PartitionAllocation
{
    std::uint32_t partition;
    std::uint32_t type;
    std::uint32_t size;
};

// One monotonic arena shared by partition allocations and thread stacks.
// Reclamation remains unsupported; neither end may cross the other.
class GuestAllocator
{
public:
    GuestAllocator(GuestAddress load_address, std::size_t memory_size, std::span<const PrxSegment> segments);
    GuestAddress allocate(std::uint32_t size, bool high);
    std::uint32_t free_size() const;
    std::uint32_t allocate_partition(std::uint32_t id, PartitionAllocation allocation);
    GuestAddress block_address(std::uint32_t id) const;

private:
    std::uint64_t heap_;
    std::uint64_t stack_top_;
    std::map<std::uint32_t, GuestAddress> blocks_;
};

} // namespace psp::detail
