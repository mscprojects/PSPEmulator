#include "runtime/ge.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace psp::detail
{

namespace
{

// GE command numbers, named as in the pinned PSPSDK's gu/guInternal.h. Each state
// command stores its 24-bit argument in the register with the same number.
constexpr std::uint32_t kNop = 0x00;
constexpr std::uint32_t kVertexAddress = 0x01;
constexpr std::uint32_t kIndexAddress = 0x02;
constexpr std::uint32_t kPrimitive = 0x04;
constexpr std::uint32_t kJump = 0x08;
constexpr std::uint32_t kEnd = 0x0C;
constexpr std::uint32_t kFinish = 0x0F;
constexpr std::uint32_t kBase = 0x10;
constexpr std::uint32_t kVertexType = 0x12;
constexpr std::uint32_t kOffsetAddress = 0x13;
constexpr std::uint32_t kRegion1 = 0x15;
constexpr std::uint32_t kRegion2 = 0x16;
constexpr std::uint32_t kLightingEnable = 0x17;
constexpr std::uint32_t kDepthClipEnable = 0x1C;
constexpr std::uint32_t kLogicOpEnable = 0x28;
constexpr std::uint32_t kShadeMode = 0x50;
constexpr std::uint32_t kFrameBufferPointer = 0x9C;
constexpr std::uint32_t kFrameBufferWidth = 0x9D;
constexpr std::uint32_t kClutLoad = 0xC4;
constexpr std::uint32_t kFrameBufferPixelFormat = 0xD2;
constexpr std::uint32_t kClearMode = 0xD3;
constexpr std::uint32_t kScissor1 = 0xD4;
constexpr std::uint32_t kScissor2 = 0xD5;
constexpr std::uint32_t kDepthMask = 0xE7;
constexpr std::uint32_t kColorMask = 0xE8;
constexpr std::uint32_t kAlphaMask = 0xE9;

GuestAddress physical(GuestAddress address)
{
    return GuestAddress{address.value_of() & 0x1FFFFFFFU};
}

void validate_pointer(const Memory &memory, GuestAddress address)
{
    if (address.value_of() == 0 || (address.value_of() & 3U) != 0)
    {
        throw std::runtime_error("GE pointer must be non-null and aligned");
    }
    memory.validate_range(address, 4);
}

// Defined GE state registers, including GU's reset list. Retaining a register
// does not enable its feature: draw() rejects unsupported active pipeline state.
bool state_register(std::uint32_t command)
{
    return command == 0x12 || command == 0x13 || (command >= 0x15 && command <= 0x28) ||
           (command >= 0x2A && command <= 0x33) || (command >= 0x36 && command <= 0x38) ||
           (command >= 0x3A && command <= 0x4D) || command == 0x50 || command == 0x51 ||
           (command >= 0x53 && command <= 0x58) || (command >= 0x5B && command <= 0xB5) ||
           (command >= 0xB8 && command <= 0xD0) || (command >= 0xD2 && command <= 0xE9) || command == 0xEB ||
           command == 0xEC || command == 0xEE;
}

} // namespace

Ge::Ge(Memory &memory, Kernel &kernel, std::uint64_t command_budget)
    : memory_(memory), kernel_(kernel), command_budget_(command_budget)
{
    if (command_budget == 0)
    {
        throw std::invalid_argument("GE command budget must be positive");
    }
}

std::uint32_t Ge::set_callback(GuestAddress address)
{
    validate_pointer(memory_, address);
    memory_.validate_range(address, 16);
    // Signal commands are outside this milestone. Validate their registration
    // too, and reject SIGNAL if a guest actually submits one.
    const auto signal = GuestAddress{memory_.read_u32(address)};
    const auto finish = GuestAddress{memory_.read_u32(GuestAddress{address.value_of() + 8})};
    for (const auto entry : {signal, finish})
    {
        if (entry.value_of() != 0)
        {
            validate_pointer(memory_, entry);
        }
    }
    for (std::size_t id = 0; id < callbacks_.size(); ++id)
    {
        if (!callbacks_[id])
        {
            callbacks_[id] = Callback{finish, GuestAddress{memory_.read_u32(GuestAddress{address.value_of() + 12})}};
            return static_cast<std::uint32_t>(id);
        }
    }
    throw std::runtime_error("GE callback slots exhausted");
}

void Ge::unset_callback(std::uint32_t id)
{
    if (!callbacks_.at(id))
    {
        throw std::runtime_error("Invalid GE callback ID");
    }
    callbacks_[id].reset();
}

std::uint32_t Ge::enqueue(const GeSubmission &submission)
{
    const auto address = physical(submission.address);
    const auto stall = physical(submission.stall);
    validate_pointer(memory_, address);
    if (stall.value_of() != 0)
    {
        validate_pointer(memory_, stall);
    }
    if (submission.arguments.value_of() != 0)
    {
        throw std::runtime_error("GE list context arguments are unsupported");
    }
    std::optional<Callback> callback;
    if (submission.callback != 0xFFFFFFFFU)
    {
        callback = callbacks_.at(submission.callback);
        if (!callback)
        {
            throw std::runtime_error("Invalid GE callback ID");
        }
    }
    for (std::size_t id = 0; id < lists_.size(); ++id)
    {
        const auto &slot = lists_[id];
        if (!slot || slot->done)
        {
            lists_[id] = List{address, stall, callback};
            queue_.push_back(static_cast<std::uint32_t>(id));
            return static_cast<std::uint32_t>(id);
        }
    }
    throw std::runtime_error("GE list slots exhausted");
}

void Ge::update_stall(std::uint32_t id, GuestAddress address)
{
    auto &list = lists_.at(id);
    if (!list || list->done)
    {
        throw std::runtime_error("GE stall update needs a live list");
    }
    address = physical(address);
    if (address.value_of() != 0)
    {
        validate_pointer(memory_, address);
    }
    list->stall = address;
}

std::optional<std::uint32_t> Ge::list_sync(std::uint32_t id, std::uint32_t mode)
{
    return sync(id, mode);
}

std::optional<std::uint32_t> Ge::draw_sync(std::uint32_t mode)
{
    return sync(std::nullopt, mode);
}

bool Ge::runnable() const
{
    if (queue_.empty())
    {
        return false;
    }
    // enqueue() initializes a slot before queuing it; END removes it before reuse.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &list = lists_[queue_.front()].value();
    return list.pc != list.stall;
}

void Ge::step()
{
    if (!runnable())
    {
        return;
    }
    const auto id = queue_.front();
    // runnable() has already read this occupied slot on the same thread.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto &list = lists_[id].value();
    const auto pc = list.pc;
    std::optional<std::uint32_t> word;
    try
    {
        if (commands_ == command_budget_)
        {
            throw std::runtime_error("GE command budget exhausted");
        }
        word = memory_.read_u32(pc);
        ++commands_;
        if (pc.value_of() > std::numeric_limits<std::uint32_t>::max() - 4)
        {
            throw std::runtime_error("GE command address overflow");
        }
        list.pc = GuestAddress{pc.value_of() + 4};
        execute(list, *word);
        if (list.done)
        {
            queue_.pop_front();
            wake_waiters();
        }
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(fmt::format("GE list {} at 0x{:x}{}: {}", id, pc.value_of(),
                                             word ? fmt::format(" (command 0x{:08x})", *word) : "", error.what()));
    }
}

bool Ge::interrupt_pending() const
{
    return !interrupts_.empty();
}

bool Ge::deliver_interrupt()
{
    if (!interrupts_.empty())
    {
        const auto &interrupt = interrupts_.front();
        if (kernel_.enter_interrupt_callback(interrupt.callback.finish, interrupt.argument, interrupt.callback.common))
        {
            interrupts_.pop_front();
            return true;
        }
    }
    return false;
}

std::uint32_t Ge::status(std::uint32_t id) const
{
    const auto &list = lists_.at(id);
    if (!list)
    {
        throw std::runtime_error("Invalid GE list ID");
    }
    if (list->done)
    {
        return 0;
    }
    if (queue_.front() != id)
    {
        return 1;
    }
    return runnable() ? 2 : 3;
}

std::optional<std::uint32_t> Ge::sync(std::optional<std::uint32_t> id, std::uint32_t mode)
{
    if (mode > 1)
    {
        throw std::runtime_error("Unsupported GE sync mode");
    }
    const auto current_status = id ? status(*id) : queue_.empty() ? 0 : status(queue_.front());
    if (mode == 1 || current_status == 0)
    {
        return current_status;
    }
    waiters_.push_back({kernel_.current_thread_id(), id});
    kernel_.wait(Kernel::Wait::Ge);
    return std::nullopt;
}

void Ge::wake_waiters()
{
    for (auto waiter = waiters_.begin(); waiter != waiters_.end();)
    {
        if (waiter->list ? status(*waiter->list) == 0 : queue_.empty())
        {
            kernel_.wake(waiter->thread, Kernel::Wait::Ge, 0);
            waiter = waiters_.erase(waiter);
        }
        else
        {
            ++waiter;
        }
    }
}

GuestAddress Ge::relative_address(std::uint32_t low) const
{
    const auto address = std::uint64_t{(registers_[kBase] & 0xF0000U) << 8} + (low & 0xFFFFFFU) +
                         (std::uint64_t{registers_[kOffsetAddress]} << 8);
    if (address > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("GE relative address overflow");
    }
    return physical(GuestAddress{static_cast<std::uint32_t>(address)});
}

void Ge::execute(List &list, std::uint32_t word)
{
    const auto command = word >> 24;
    const auto value = word & 0xFFFFFFU;
    switch (command)
    {
    case kNop:
        return;
    case kVertexAddress:
        vertices_ = relative_address(value);
        return;
    case kIndexAddress: // Indexed drawing is rejected by vertex format validation.
    case kBase:
        registers_[command] = value;
        return;
    case kPrimitive:
        draw(value);
        return;
    case kJump: // Includes sceGuGetMemory's inline vertex data.
        list.pc = relative_address(value);
        validate_pointer(memory_, list.pc);
        return;
    case kEnd:
        if (!list.finished)
        {
            throw std::runtime_error("GE END without FINISH is unsupported");
        }
        list.done = true;
        return;
    case kFinish:
        list.finished = true;
        if (list.callback && list.callback->finish.value_of() != 0)
        {
            interrupts_.push_back({*list.callback, value & 0xFFFFU});
        }
        return;
    case kClutLoad: // A zero-block reset has no upload.
        if (value != 0)
        {
            throw std::runtime_error("GE CLUT loading is unsupported");
        }
        break;
    default:
        if (!state_register(command))
        {
            throw std::runtime_error(fmt::format("Unsupported GE command 0x{:x}", command));
        }
        break;
    }
    registers_[command] = value;
}

void Ge::draw(std::uint32_t primitive)
{
    const auto count = primitive & 0xFFFFU;
    const auto type = primitive >> 16;
    const auto clearing = (registers_[kClearMode] & 1U) != 0;
    const auto format = registers_[kVertexType];
    const bool floating = format == 0x80019CU; // RGBA8888 + float XYZ + through mode.
    if ((!floating && format != 0x80011CU) ||
        (clearing ? floating || type != 6 || count != 2 : type != 3 || count != 3))
    {
        throw std::runtime_error("GE supports one unindexed 2D triangle or one clear sprite per PRIM");
    }
    if (registers_[kFrameBufferPixelFormat] != 3 || registers_[kShadeMode] > 1 || registers_[kColorMask] != 0 ||
        registers_[kAlphaMask] != 0 ||
        (clearing ? registers_[kClearMode] != 0x101 && registers_[kClearMode] != 0x301 : registers_[kDepthMask] != 1))
    {
        throw std::runtime_error("Unsupported GE framebuffer, shade, mask, or depth state");
    }
    // Every enable flag except depth clipping selects an unsupported pipeline feature.
    for (auto command = kLightingEnable; command <= kLogicOpEnable; ++command)
    {
        if (command != kDepthClipEnable && registers_[command] != 0)
        {
            throw std::runtime_error(fmt::format("Unsupported enabled GE feature 0x{:x}", command));
        }
    }
    // Region and scissor bounds both clip to the LCD.
    const RenderTarget target{
        .framebuffer = GuestAddress{0x04000000U | (registers_[kFrameBufferPointer] & 0x1FFFF0U)},
        .stride = registers_[kFrameBufferWidth] & 0x7FCU,
        .left = std::max(static_cast<int>(registers_[kRegion1] & 1023), static_cast<int>(registers_[kScissor1] & 1023)),
        .top = std::max(static_cast<int>((registers_[kRegion1] >> 10) & 1023),
                        static_cast<int>((registers_[kScissor1] >> 10) & 1023)),
        .right = std::min({kLcdWidth - 1, static_cast<int>(registers_[kRegion2] & 1023),
                           static_cast<int>(registers_[kScissor2] & 1023)}),
        .bottom = std::min({kLcdHeight - 1, static_cast<int>((registers_[kRegion2] >> 10) & 1023),
                            static_cast<int>((registers_[kScissor2] >> 10) & 1023)})};
    if (target.stride == 0 || target.stride <= static_cast<std::uint32_t>(target.right))
    {
        throw std::runtime_error("GE framebuffer stride is too small");
    }
    memory_.validate_range(target.framebuffer, std::size_t{target.stride} * kLcdHeight * 4);
    const std::size_t vertex_size = floating ? 16 : 12;
    validate_pointer(memory_, vertices_);
    memory_.validate_range(vertices_, count * vertex_size);
    // Decode all input before any pixel write, including NaN/range validation.
    std::array<RasterVertex, 3> decoded{};
    for (std::size_t index = 0; index < count; ++index)
    {
        decoded[index] =
            read_vertex(GuestAddress{vertices_.value_of() + static_cast<std::uint32_t>(index * vertex_size)}, floating);
    }
    if (clearing)
    {
        const auto channels =
            (registers_[kClearMode] & 0x200U) != 0 ? ClearChannels::ColorAndAlpha : ClearChannels::Color;
        draw_clear(memory_, target, {decoded[0], decoded[1]}, channels);
    }
    else
    {
        draw_triangle(memory_, target, decoded, registers_[kShadeMode] == 1 ? Shading::Smooth : Shading::Flat);
    }
    vertices_ = GuestAddress{vertices_.value_of() + static_cast<std::uint32_t>(count * vertex_size)};
}

RasterVertex Ge::read_vertex(GuestAddress address, bool floating) const
{
    const auto coordinate = [&](std::uint32_t offset)
    {
        const auto pointer = GuestAddress{address.value_of() + offset};
        if (!floating)
        {
            return static_cast<std::int32_t>(memory_.read_u16(pointer)) * 16;
        }
        const auto value = std::bit_cast<float>(memory_.read_u32(pointer));
        if (!std::isfinite(value) || std::abs(value) > 65535.0F)
        {
            throw std::runtime_error("GE coordinate is nonfinite or outside the supported range");
        }
        return static_cast<std::int32_t>(value * 16); // Provisional 1/16-pixel truncation.
    };
    return {coordinate(4), coordinate(floating ? 8 : 6), memory_.read_u32(address)};
}

} // namespace psp::detail
