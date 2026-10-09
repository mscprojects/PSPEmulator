#pragma once

#include "memory/memory.hpp"

#include <optional>
#include <span>

namespace psp::detail
{

struct Framebuffer
{
    GuestAddress address{0};
    std::uint32_t stride{};
    std::uint32_t pixel_format{3};
};

// SDL-independent LCD state. Captured frames are packed RGBA bytes with opaque alpha.
class Display
{
public:
    static constexpr int width = 480;
    static constexpr int height = 272;

    explicit Display(const Memory &memory);
    void set_mode(std::uint32_t mode, std::uint32_t width, std::uint32_t height);
    // Sync 0 approximates next-hsync selection by applying it immediately;
    // sync 1 activates at vblank. A null address disables the framebuffer.
    void set_framebuffer(const Framebuffer &framebuffer, std::uint32_t sync);
    void vblank();
    // Capture active contents without activating a pending next-frame selection.
    void capture();
    std::span<const std::uint8_t> pixels() const;

private:
    const Memory &memory_;
    Framebuffer active_;
    std::optional<Framebuffer> pending_;
    Payload pixels_;
    bool captured_framebuffer_{};
};

} // namespace psp::detail
