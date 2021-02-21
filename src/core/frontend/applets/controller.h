// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>

#include "common/common_types.h"

namespace Service::SM {
class ServiceManager;
}

namespace Core::Frontend {

using BorderColor = std::array<u8, 4>;
using ExplainText = std::array<char, 0x81>;

struct ControllerParameters {
    s8 min_players{};
    s8 max_players{};
    bool keep_controllers_connected{};
    bool enable_single_mode{};
    bool enable_border_color{};
    std::vector<BorderColor> border_colors{};
    bool enable_explain_text{};
    std::vector<ExplainText> explain_text{};
    bool allow_pro_controller{};
    bool allow_handheld{};
    bool allow_dual_joycons{};
    bool allow_left_joycon{};
    bool allow_right_joycon{};
    bool allow_gamecube_controller{};
};

class ControllerApplet {
public:
    virtual ~ControllerApplet();

    virtual void ReconfigureControllers(std::function<void()> callback,
                                        const ControllerParameters& parameters) const = 0;
};

class DefaultControllerApplet final : public ControllerApplet {
public:
    explicit DefaultControllerApplet(Service::SM::ServiceManager& service_manager_);
    ~DefaultControllerApplet() override;

    void ReconfigureControllers(std::function<void()> callback,
                                const ControllerParameters& parameters) const override;

private:
    Service::SM::ServiceManager& service_manager;
};

} // namespace Core::Frontend
