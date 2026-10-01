#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace psp
{

// An owning byte sequence.
using Payload = std::vector<std::uint8_t>;

// A read-only borrowed view; its backing storage must outlive readers and parsed results.
using PayloadSpan = std::span<const std::uint8_t>;

} // namespace psp
