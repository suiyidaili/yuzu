// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "common/common_types.h"
#include "core/core_timing.h"
#include "core/hle/service/hid/controllers/gesture.h"

namespace Service::HID {
constexpr std::size_t SHARED_MEMORY_OFFSET = 0x3BA00;

Controller_Gesture::Controller_Gesture(Core::System& system) : ControllerBase(system) {}
Controller_Gesture::~Controller_Gesture() = default;

void Controller_Gesture::OnInit() {}

void Controller_Gesture::OnRelease() {}

void Controller_Gesture::OnUpdate(const Core::Timing::CoreTiming& core_timing, u8* data,
                                  std::size_t size) {
    shared_memory.header.timestamp = core_timing.GetCPUTicks();
    shared_memory.header.total_entry_count = 17;

    if (!IsControllerActivated()) {
        shared_memory.header.entry_count = 0;
        shared_memory.header.last_entry_index = 0;
        return;
    }
    shared_memory.header.entry_count = 16;

    const auto& last_entry = shared_memory.gesture_states[shared_memory.header.last_entry_index];
    shared_memory.header.last_entry_index = (shared_memory.header.last_entry_index + 1) % 17;
    auto& cur_entry = shared_memory.gesture_states[shared_memory.header.last_entry_index];

    cur_entry.sampling_number = last_entry.sampling_number + 1;
    cur_entry.sampling_number2 = cur_entry.sampling_number;
    // TODO(ogniK): Update gesture states

    std::memcpy(data + SHARED_MEMORY_OFFSET, &shared_memory, sizeof(SharedMemory));
}

void Controller_Gesture::OnLoadInputDevices() {}
} // namespace Service::HID
