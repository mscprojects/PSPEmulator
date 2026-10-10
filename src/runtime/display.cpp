#include "runtime/display.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace psp::detail
{

Display::Display(const Memory &memory) : memory_(memory), pixels_(std::size_t{width} * height * 4)
{
    for (std::size_t alpha = 3; alpha < pixels_.size(); alpha += 4)
    {
        pixels_[alpha] = 255;
    }
}

// Mode and dimensions are independent PSP ABI fields.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Display::set_mode(std::uint32_t mode, std::uint32_t requested_width, std::uint32_t requested_height)
{
    if (mode != 0 || requested_width != width || requested_height != height)
    {
        throw std::runtime_error("Unsupported display mode; expected LCD 480x272");
    }
}

void Display::set_framebuffer(const Framebuffer &framebuffer, std::uint32_t sync)
{
    if (sync > 1 || framebuffer.pixel_format != 3)
    {
        throw std::runtime_error("Unsupported framebuffer format or synchronization; expected RGBA 8888, sync 0 or 1");
    }
    if (framebuffer.address.value_of() != 0)
    {
        if ((framebuffer.address.value_of() & 15U) != 0 || framebuffer.stride < width ||
            !std::has_single_bit(framebuffer.stride))
        {
            throw std::runtime_error("Invalid framebuffer address alignment or stride");
        }
        const auto size = static_cast<std::uint64_t>(framebuffer.stride) * height * 4;
        if (size > (std::uint64_t{1} << 32) - framebuffer.address.value_of())
        {
            throw std::runtime_error("Framebuffer exceeds guest address space");
        }
        memory_.validate_range(framebuffer.address, static_cast<std::size_t>(size));
    }
    if (sync == 1)
    {
        pending_ = framebuffer;
    }
    else
    {
        active_ = framebuffer;
        pending_.reset();
    }
}

void Display::vblank()
{
    if (pending_)
    {
        active_ = *pending_;
        pending_.reset();
    }
}

void Display::capture()
{
    if (active_.address.value_of() == 0)
    {
        if (!captured_framebuffer_)
        {
            return;
        }
        std::fill(pixels_.begin(), pixels_.end(), 0);
    }
    else
    {
        for (std::uint32_t row = 0; row < height; ++row)
        {
            const auto address = GuestAddress{active_.address.value_of() + row * active_.stride * 4};
            auto destination = std::span{pixels_}.subspan(std::size_t{row} * width * 4, std::size_t{width} * 4);
            memory_.read_into(address, std::as_writable_bytes(destination));
        }
    }
    for (std::size_t alpha = 3; alpha < pixels_.size(); alpha += 4)
    {
        pixels_[alpha] = 255;
    }
    captured_framebuffer_ = active_.address.value_of() != 0;
}

std::span<const std::uint8_t> Display::pixels() const
{
    return pixels_;
}

} // namespace psp::detail
