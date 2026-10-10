#pragma once

#include <cstdint>
#include <vector>

namespace psp::detail
{

// Half-open guest address range. 64-bit bounds represent ranges ending at 2^32.
struct AddressRange
{
    std::uint64_t begin;
    std::uint64_t end;
    bool operator==(const AddressRange &) const = default;
};

enum class AllocationDirection : std::uint8_t
{
    Low,
    High,
};

// Address-ordered free ranges for guest allocations, with 256-byte alignment.
// Low allocations take the lowest suitable range; high allocations take the highest.
class AddressArena
{
public:
    // Manage [begin, end) after rounding begin up and end down to the alignment, which can
    // leave no free space. Throws std::invalid_argument if begin > end or end > 2^32.
    AddressArena(std::uint64_t begin, std::uint64_t end);
    // Round size up to the alignment. Throws std::runtime_error for a zero size or when
    // no free range fits; a failed allocation leaves the arena unchanged.
    AddressRange allocate(std::uint32_t size, AllocationDirection direction);
    // Return a live range from allocate(), merging adjacent free ranges. Throws
    // std::logic_error without changes for an empty range or one overlapping free space,
    // such as a repeated free; the caller must not return ranges it never allocated.
    void free(AddressRange range);
    std::uint64_t free_size() const;
    std::uint64_t largest_free_size() const;

private:
    std::vector<AddressRange> free_ranges_;
};

} // namespace psp::detail
