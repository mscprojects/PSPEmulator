#include "runtime/controller.hpp"

#include <stdexcept>
#include <utility>

namespace psp::detail
{

Controller::Controller(Memory &memory, Kernel &kernel) : memory_(memory), kernel_(kernel)
{
}

void Controller::set_input(ControllerState input)
{
    input_ = input;
}

std::uint32_t Controller::set_sampling_cycle(std::uint32_t cycle)
{
    if (cycle != 0)
    {
        throw std::runtime_error("Only vblank controller sampling (cycle 0) is supported");
    }
    return 0;
}

std::uint32_t Controller::set_sampling_mode(std::uint32_t mode)
{
    if (mode > 1)
    {
        throw std::runtime_error("Unsupported controller sampling mode");
    }
    return std::exchange(mode_, mode);
}

std::optional<std::uint32_t> Controller::read_positive(GuestAddress address, std::uint32_t count)
{
    if (count != 1 || (address.value_of() & 3U) != 0)
    {
        throw std::runtime_error("Controller reads require one aligned SceCtrlData buffer");
    }
    memory_.validate_range(address, sizeof(GuestControllerData));
    if (sample_)
    {
        write_controller_data(memory_, address, *sample_);
        sample_.reset();
        return 1;
    }
    readers_.push_back({kernel_.current_thread_id(), address});
    kernel_.wait_controller();
    return std::nullopt;
}

void Controller::vblank()
{
    GuestControllerData data;
    data.timestamp = static_cast<std::uint32_t>(kernel_.system_time());
    data.buttons = input_.buttons;
    if (mode_ == 1)
    {
        data.left_x = input_.left_x;
        data.left_y = input_.left_y;
        data.right_x = input_.right_x;
        data.right_y = input_.right_y;
    }
    if (readers_.empty())
    {
        // Keep only the latest unread sample; a historical ring buffer is deferred.
        sample_ = data;
        return;
    }
    const auto reader = readers_.front();
    write_controller_data(memory_, reader.address, data);
    kernel_.wake_controller(reader.thread);
    readers_.pop_front();
}

} // namespace psp::detail
