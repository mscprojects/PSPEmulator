#pragma once

#include "runtime/synchronization.hpp"
#include "runtime/threads.hpp"

namespace psp::detail
{

// Owns kernel components and their shared object-ID namespace. The allocator
// owns partition blocks; the kernel assigns their IDs alongside thread/sync IDs.
class Kernel
{
    std::uint32_t next_id_{1};
    GuestAllocator &allocator_;

public:
    Kernel(Memory &memory, GuestAllocator &allocator, GuestAddress global_pointer);
    // Components refer to this kernel's ID counter; their owner must stay put.
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;
    std::uint32_t allocate_partition(PartitionAllocation allocation);

    Threads threads;
    Synchronization synchronization;
};

} // namespace psp::detail
