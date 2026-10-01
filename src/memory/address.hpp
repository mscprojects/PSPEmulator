#pragma once

#include <strong_type/equality.hpp>
#include <strong_type/type.hpp>

#include <cstdint>

namespace psp
{

using GuestAddress = strong::type<std::uint32_t, struct GuestAddressTag, strong::equality>;

} // namespace psp
