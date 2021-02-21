// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/kernel/object.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class SharedMemory;
}

namespace Service::HID {

class IRS final : public ServiceFramework<IRS> {
public:
    explicit IRS(Core::System& system_);
    ~IRS() override;

private:
    void ActivateIrsensor(Kernel::HLERequestContext& ctx);
    void DeactivateIrsensor(Kernel::HLERequestContext& ctx);
    void GetIrsensorSharedMemoryHandle(Kernel::HLERequestContext& ctx);
    void StopImageProcessor(Kernel::HLERequestContext& ctx);
    void RunMomentProcessor(Kernel::HLERequestContext& ctx);
    void RunClusteringProcessor(Kernel::HLERequestContext& ctx);
    void RunImageTransferProcessor(Kernel::HLERequestContext& ctx);
    void GetImageTransferProcessorState(Kernel::HLERequestContext& ctx);
    void RunTeraPluginProcessor(Kernel::HLERequestContext& ctx);
    void GetNpadIrCameraHandle(Kernel::HLERequestContext& ctx);
    void RunPointingProcessor(Kernel::HLERequestContext& ctx);
    void SuspendImageProcessor(Kernel::HLERequestContext& ctx);
    void CheckFirmwareVersion(Kernel::HLERequestContext& ctx);
    void SetFunctionLevel(Kernel::HLERequestContext& ctx);
    void RunImageTransferExProcessor(Kernel::HLERequestContext& ctx);
    void RunIrLedProcessor(Kernel::HLERequestContext& ctx);
    void StopImageProcessorAsync(Kernel::HLERequestContext& ctx);
    void ActivateIrsensorWithFunctionLevel(Kernel::HLERequestContext& ctx);

    std::shared_ptr<Kernel::SharedMemory> shared_mem;
    const u32 device_handle{0xABCD};
};

class IRS_SYS final : public ServiceFramework<IRS_SYS> {
public:
    explicit IRS_SYS(Core::System& system);
    ~IRS_SYS() override;
};

} // namespace Service::HID
