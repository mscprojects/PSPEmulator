#pragma once

#include "memory/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psp::detail
{

// Byte-backed words keep guest structures little-endian on every host. Guest
// pointers use these 32-bit words too; native pointers never enter the layout.
struct GuestWord
{
    std::array<std::uint8_t, 4> bytes{};

    GuestWord() = default;
    GuestWord(std::uint32_t value);
    std::uint32_t value() const;
};

enum class ThreadStatus : std::uint8_t
{
    Running = 1,
    Ready = 2,
    Waiting = 4,
    Stopped = 16,
};

// SceKernelThreadInfo, as defined by PSPSDK's src/user/pspthreadman.h.
// The current runtime supports this 104-byte version only.
struct GuestThreadInfo
{
    GuestWord size{104};
    std::array<char, 32> name{};
    GuestWord attributes;
    GuestWord status;
    GuestWord entry;
    GuestWord stack;
    GuestWord stack_size;
    GuestWord global_pointer;
    GuestWord initial_priority;
    GuestWord current_priority;
    GuestWord wait_type;
    GuestWord wait_id;
    GuestWord wakeup_count;
    GuestWord exit_status;
    GuestWord run_clocks_low;
    GuestWord run_clocks_high;
    GuestWord interrupt_preempt_count;
    GuestWord thread_preempt_count;
    GuestWord release_count;
};

// SceLwMutexWorkarea. Guest code can access these fields directly.
struct GuestMutexWorkArea
{
    GuestWord lock_count;
    GuestWord owner;
    GuestWord attributes;
    GuestWord waiter_count;
    GuestWord id;
    std::array<GuestWord, 3> padding{};
};

// SceCtrlData from PSPSDK's pspctrl.h, including the optional second stick.
struct GuestControllerData
{
    GuestWord timestamp;
    GuestWord buttons;
    std::uint8_t left_x{128};
    std::uint8_t left_y{128};
    std::uint8_t right_x{128};
    std::uint8_t right_y{128};
    std::array<std::uint8_t, 4> reserved{};
};

// PspGeCallbackData from PSPSDK's ge/pspge.h: signal and finish handlers with their arguments.
struct GuestGeCallbackData
{
    GuestWord signal_function;
    GuestWord signal_argument;
    GuestWord finish_function;
    GuestWord finish_argument;
};

static_assert(sizeof(GuestWord) == 4);
static_assert(sizeof(GuestThreadInfo) == 104);
static_assert(offsetof(GuestThreadInfo, stack) == 48);
static_assert(offsetof(GuestThreadInfo, stack_size) == 52);
static_assert(offsetof(GuestThreadInfo, exit_status) == 80);
static_assert(sizeof(GuestMutexWorkArea) == 32);
static_assert(offsetof(GuestMutexWorkArea, id) == 16);
static_assert(std::is_trivially_copyable_v<GuestThreadInfo>);
static_assert(std::is_trivially_copyable_v<GuestMutexWorkArea>);
static_assert(sizeof(GuestControllerData) == 16);
static_assert(offsetof(GuestControllerData, left_x) == 8);
static_assert(std::is_trivially_copyable_v<GuestControllerData>);
static_assert(sizeof(GuestGeCallbackData) == 16);
static_assert(offsetof(GuestGeCallbackData, finish_function) == 8);
static_assert(std::is_trivially_copyable_v<GuestGeCallbackData>);

// Reads and writes validate the entire guest range before copying any bytes.
void write_thread_info(Memory &memory, GuestAddress address, const GuestThreadInfo &info);
GuestMutexWorkArea read_mutex_work_area(const Memory &memory, GuestAddress address);
void write_mutex_work_area(Memory &memory, GuestAddress address, const GuestMutexWorkArea &work_area);
void write_controller_data(Memory &memory, GuestAddress address, const GuestControllerData &data);
GuestGeCallbackData read_ge_callback_data(const Memory &memory, GuestAddress address);

} // namespace psp::detail
