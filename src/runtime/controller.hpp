#pragma once

#include "runtime/controller_state.hpp"
#include "runtime/kernel.hpp"

#include <deque>
#include <optional>

namespace psp::detail
{

// Samples host state on guest vblank. A sample is consumed by one reader;
// readers without fresh data wait in FIFO order, allowing other threads to run.
class Controller
{
    struct Reader
    {
        std::uint32_t thread;
        GuestAddress address;
    };

public:
    Controller(Memory &memory, Kernel &kernel);
    void set_input(ControllerState input);
    std::uint32_t set_sampling_cycle(std::uint32_t cycle);
    std::uint32_t set_sampling_mode(std::uint32_t mode);
    std::optional<std::uint32_t> read_positive(GuestAddress address, std::uint32_t count);
    void vblank();

private:
    Memory &memory_;
    Kernel &kernel_;
    ControllerState input_;
    std::uint32_t mode_{};
    std::optional<GuestControllerData> sample_;
    std::deque<Reader> readers_;
};

} // namespace psp::detail
