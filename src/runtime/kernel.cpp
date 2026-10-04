#include "runtime/kernel.hpp"

namespace psp::detail
{

Kernel::Kernel(Memory &memory, GuestAllocator &allocator, GuestAddress global_pointer)
    : allocator_(allocator), threads(memory, allocator, next_id_, global_pointer), synchronization(memory, next_id_)
{
}

std::uint32_t Kernel::allocate_partition(PartitionAllocation allocation)
{
    return allocator_.allocate_partition(next_id_++, allocation);
}

} // namespace psp::detail
