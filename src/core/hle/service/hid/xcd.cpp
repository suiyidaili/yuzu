// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/hid/xcd.h"

namespace Service::HID {

XCD_SYS::XCD_SYS(Core::System& system_) : ServiceFramework{system_, "xcd:sys"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetDataFormat"},
        {1, nullptr, "SetDataFormat"},
        {2, nullptr, "GetMcuState"},
        {3, nullptr, "SetMcuState"},
        {4, nullptr, "GetMcuVersionForNfc"},
        {5, nullptr, "CheckNfcDevicePower"},
        {10, nullptr, "SetNfcEvent"},
        {11, nullptr, "GetNfcInfo"},
        {12, nullptr, "StartNfcDiscovery"},
        {13, nullptr, "StopNfcDiscovery"},
        {14, nullptr, "StartNtagRead"},
        {15, nullptr, "StartNtagWrite"},
        {16, nullptr, "SendNfcRawData"},
        {17, nullptr, "RegisterMifareKey"},
        {18, nullptr, "ClearMifareKey"},
        {19, nullptr, "StartMifareRead"},
        {20, nullptr, "StartMifareWrite"},
        {101, nullptr, "GetAwakeTriggerReasonForLeftRail"},
        {102, nullptr, "GetAwakeTriggerReasonForRightRail"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

XCD_SYS::~XCD_SYS() = default;

} // namespace Service::HID
