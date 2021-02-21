// Copyright 2019 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <map>
#include "common/common_types.h"

namespace Core::Timing {
class CoreTiming;
}

namespace Service::APM {

enum class PerformanceConfiguration : u32 {
    Config1 = 0x00010000,
    Config2 = 0x00010001,
    Config3 = 0x00010002,
    Config4 = 0x00020000,
    Config5 = 0x00020001,
    Config6 = 0x00020002,
    Config7 = 0x00020003,
    Config8 = 0x00020004,
    Config9 = 0x00020005,
    Config10 = 0x00020006,
    Config11 = 0x92220007,
    Config12 = 0x92220008,
    Config13 = 0x92220009,
    Config14 = 0x9222000A,
    Config15 = 0x9222000B,
    Config16 = 0x9222000C,
};

enum class CpuBoostMode : u32 {
    Disabled = 0,
    Full = 1,    // CPU + GPU -> Config 13, 14, 15, or 16
    Partial = 2, // GPU Only -> Config 15 or 16
};

enum class PerformanceMode : u8 {
    Handheld = 0,
    Docked = 1,
};

// Class to manage the state and change of the emulated system performance.
// Specifically, this deals with PerformanceMode, which corresponds to the system being docked or
// undocked, and PerformanceConfig which specifies the exact CPU, GPU, and Memory clocks to operate
// at. Additionally, this manages 'Boost Mode', which allows games to temporarily overclock the
// system during times of high load -- this simply maps to different PerformanceConfigs to use.
class Controller {
public:
    explicit Controller(Core::Timing::CoreTiming& core_timing);
    ~Controller();

    void SetPerformanceConfiguration(PerformanceMode mode, PerformanceConfiguration config);
    void SetFromCpuBoostMode(CpuBoostMode mode);

    PerformanceMode GetCurrentPerformanceMode() const;
    PerformanceConfiguration GetCurrentPerformanceConfiguration(PerformanceMode mode);

private:
    void SetClockSpeed(u32 mhz);

    [[maybe_unused]] Core::Timing::CoreTiming& core_timing;

    std::map<PerformanceMode, PerformanceConfiguration> configs;
};

} // namespace Service::APM
