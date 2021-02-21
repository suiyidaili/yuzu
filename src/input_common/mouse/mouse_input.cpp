// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "core/settings.h"
#include "input_common/mouse/mouse_input.h"

namespace MouseInput {

Mouse::Mouse() {
    update_thread = std::thread(&Mouse::UpdateThread, this);
}

Mouse::~Mouse() {
    update_thread_running = false;
    if (update_thread.joinable()) {
        update_thread.join();
    }
}

void Mouse::UpdateThread() {
    constexpr int update_time = 10;
    while (update_thread_running) {
        for (MouseInfo& info : mouse_info) {
            const Common::Vec3f angular_direction{
                -info.tilt_direction.y,
                0.0f,
                -info.tilt_direction.x,
            };

            info.motion.SetGyroscope(angular_direction * info.tilt_speed);
            info.motion.UpdateRotation(update_time * 1000);
            info.motion.UpdateOrientation(update_time * 1000);
            info.tilt_speed = 0;
            info.data.motion = info.motion.GetMotion();
            if (Settings::values.mouse_panning) {
                info.last_mouse_change *= 0.96f;
                info.data.axis = {static_cast<int>(16 * info.last_mouse_change.x),
                                  static_cast<int>(16 * -info.last_mouse_change.y)};
            }
        }
        if (configuring) {
            UpdateYuzuSettings();
        }
        if (mouse_panning_timout++ > 20) {
            StopPanning();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(update_time));
    }
}

void Mouse::UpdateYuzuSettings() {
    if (buttons == 0) {
        return;
    }

    mouse_queue.Push(MouseStatus{
        .button = last_button,
    });
}

void Mouse::PressButton(int x, int y, int button_) {
    const auto button_index = static_cast<std::size_t>(button_);
    if (button_index >= mouse_info.size()) {
        return;
    }

    const auto button = 1U << button_index;
    buttons |= static_cast<u16>(button);
    last_button = static_cast<MouseButton>(button_index);

    mouse_info[button_index].mouse_origin = Common::MakeVec(x, y);
    mouse_info[button_index].last_mouse_position = Common::MakeVec(x, y);
    mouse_info[button_index].data.pressed = true;
}

void Mouse::StopPanning() {
    for (MouseInfo& info : mouse_info) {
        if (Settings::values.mouse_panning) {
            info.data.axis = {};
            info.tilt_speed = 0;
            info.last_mouse_change = {};
        }
    }
}

void Mouse::MouseMove(int x, int y, int center_x, int center_y) {
    for (MouseInfo& info : mouse_info) {
        if (Settings::values.mouse_panning) {
            auto mouse_change =
                (Common::MakeVec(x, y) - Common::MakeVec(center_x, center_y)).Cast<float>();
            mouse_panning_timout = 0;

            if (mouse_change.y == 0 && mouse_change.x == 0) {
                continue;
            }
            const auto mouse_change_length = mouse_change.Length();
            if (mouse_change_length < 3.0f) {
                mouse_change /= mouse_change_length / 3.0f;
            }

            info.last_mouse_change = (info.last_mouse_change * 0.91f) + (mouse_change * 0.09f);

            const auto last_mouse_change_length = info.last_mouse_change.Length();
            if (last_mouse_change_length > 8.0f) {
                info.last_mouse_change /= last_mouse_change_length / 8.0f;
            } else if (last_mouse_change_length < 1.0f) {
                info.last_mouse_change = mouse_change / mouse_change.Length();
            }

            info.tilt_direction = info.last_mouse_change;
            info.tilt_speed = info.tilt_direction.Normalize() * info.sensitivity;
            continue;
        }

        if (info.data.pressed) {
            const auto mouse_move = Common::MakeVec(x, y) - info.mouse_origin;
            const auto mouse_change = Common::MakeVec(x, y) - info.last_mouse_position;
            info.last_mouse_position = Common::MakeVec(x, y);
            info.data.axis = {mouse_move.x, -mouse_move.y};

            if (mouse_change.x == 0 && mouse_change.y == 0) {
                info.tilt_speed = 0;
            } else {
                info.tilt_direction = mouse_change.Cast<float>();
                info.tilt_speed = info.tilt_direction.Normalize() * info.sensitivity;
            }
        }
    }
}

void Mouse::ReleaseButton(int button_) {
    const auto button_index = static_cast<std::size_t>(button_);
    if (button_index >= mouse_info.size()) {
        return;
    }

    const auto button = 1U << button_index;
    buttons &= static_cast<u16>(0xFF - button);

    mouse_info[button_index].tilt_speed = 0;
    mouse_info[button_index].data.pressed = false;
    mouse_info[button_index].data.axis = {0, 0};
}

void Mouse::BeginConfiguration() {
    buttons = 0;
    last_button = MouseButton::Undefined;
    mouse_queue.Clear();
    configuring = true;
}

void Mouse::EndConfiguration() {
    buttons = 0;
    last_button = MouseButton::Undefined;
    mouse_queue.Clear();
    configuring = false;
}

Common::SPSCQueue<MouseStatus>& Mouse::GetMouseQueue() {
    return mouse_queue;
}

const Common::SPSCQueue<MouseStatus>& Mouse::GetMouseQueue() const {
    return mouse_queue;
}

MouseData& Mouse::GetMouseState(std::size_t button) {
    return mouse_info[button].data;
}

const MouseData& Mouse::GetMouseState(std::size_t button) const {
    return mouse_info[button].data;
}
} // namespace MouseInput
