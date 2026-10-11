#pragma once

#include "memory/address.hpp"
#include "runtime/address_arena.hpp"

#include <map>

namespace psp::detail
{

struct PartitionAllocation
{
    std::uint32_t partition;
    std::uint32_t type;
    std::uint32_t size;
};

// Owns partition block IDs and ranges. The arena is shared with thread stacks
// and the return sentinel; freeing a block never clears its guest memory.
class PartitionMemory
{
public:
    // next_id is shared with threads, callbacks, and synchronization objects.
    PartitionMemory(AddressArena &arena, std::uint32_t &next_id);
    PartitionMemory(const PartitionMemory &) = delete;
    PartitionMemory &operator=(const PartitionMemory &) = delete;
    std::uint32_t allocate(PartitionAllocation allocation);
    void free(std::uint32_t id);
    std::uint32_t free_size() const;
    std::uint32_t largest_free_size() const;
    GuestAddress address(std::uint32_t id) const;

private:
    AddressArena &arena_;
    std::uint32_t &next_id_;
    std::map<std::uint32_t, AddressRange> blocks_;
};

} // namespace psp::detail
