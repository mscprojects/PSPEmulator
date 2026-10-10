#pragma once

#include "runtime/kernel.hpp"

#include <array>
#include <deque>
#include <optional>

namespace psp::detail
{

struct GeSubmission
{
    GuestAddress address;
    GuestAddress stall;
    std::uint32_t callback;
    GuestAddress arguments;
};

// Incremental command processor and bounded software renderer. State registers
// persist across lists. Only unindexed, untextured 2D triangles and clear sprites
// are rendered; active unsupported features fail before modifying VRAM.
class Ge
{
    struct Callback
    {
        GuestAddress finish;
        GuestAddress common;
    };

    struct Interrupt
    {
        Callback callback;
        std::uint32_t argument;
    };

    struct List
    {
        GuestAddress pc;
        GuestAddress stall;
        std::optional<Callback> callback;
        bool finished{};
        bool done{};
    };

    struct Waiter
    {
        std::uint32_t thread;
        std::optional<std::uint32_t> list;
    };

    struct Vertex
    {
        std::int32_t x;
        std::int32_t y;
        std::uint32_t color;
    };

public:
    Ge(Memory &memory, Kernel &kernel, std::uint64_t command_budget);
    std::uint32_t set_callback(GuestAddress address);
    void unset_callback(std::uint32_t id);
    std::uint32_t enqueue(const GeSubmission &submission);
    void update_stall(std::uint32_t id, GuestAddress address);
    std::optional<std::uint32_t> list_sync(std::uint32_t id, std::uint32_t mode);
    std::optional<std::uint32_t> draw_sync(std::uint32_t mode);
    bool runnable() const;
    // Execute at most one command. The separate command budget also bounds idle
    // GPU loops, which cannot consume the CPU instruction budget.
    void step();
    bool interrupt_pending() const;
    bool deliver_interrupt();

private:
    std::uint32_t status(std::uint32_t id) const;
    std::optional<std::uint32_t> sync(std::optional<std::uint32_t> id, std::uint32_t mode);
    void wake_waiters();
    GuestAddress relative_address(std::uint32_t low) const;
    void execute(List &list, std::uint32_t word);
    void draw(std::uint32_t primitive);
    Vertex read_vertex(GuestAddress address, bool floating) const;
    void triangle(std::array<Vertex, 3> vertices);
    void clear(const std::array<Vertex, 2> &vertices);
    void write_pixel(int x, int y, std::uint32_t color, bool clear);

    Memory &memory_;
    Kernel &kernel_;
    std::uint64_t command_budget_;
    std::uint64_t commands_{};
    std::array<std::optional<Callback>, 16> callbacks_{};
    std::array<std::optional<List>, 64> lists_{};
    std::deque<std::uint32_t> queue_;
    std::deque<Interrupt> interrupts_;
    std::deque<Waiter> waiters_;
    std::array<std::uint32_t, 256> registers_{};
    GuestAddress vertices_{0};
    GuestAddress framebuffer_{0};
    std::uint32_t stride_{};
    std::array<int, 4> clip_{}; // Inclusive left, top, right, bottom.
};

} // namespace psp::detail
