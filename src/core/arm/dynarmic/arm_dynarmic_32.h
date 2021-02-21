// Copyright 2020 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <unordered_map>

#include <dynarmic/A32/a32.h>
#include <dynarmic/A64/a64.h>
#include <dynarmic/exclusive_monitor.h>
#include "common/common_types.h"
#include "common/hash.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exclusive_monitor.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

class CPUInterruptHandler;
class DynarmicCallbacks32;
class DynarmicCP15;
class DynarmicExclusiveMonitor;
class System;

class ARM_Dynarmic_32 final : public ARM_Interface {
public:
    ARM_Dynarmic_32(System& system, CPUInterrupts& interrupt_handlers, bool uses_wall_clock,
                    ExclusiveMonitor& exclusive_monitor, std::size_t core_index);
    ~ARM_Dynarmic_32() override;

    void SetPC(u64 pc) override;
    u64 GetPC() const override;
    u64 GetReg(int index) const override;
    void SetReg(int index, u64 value) override;
    u128 GetVectorReg(int index) const override;
    void SetVectorReg(int index, u128 value) override;
    u32 GetPSTATE() const override;
    void SetPSTATE(u32 pstate) override;
    void Run() override;
    void ExceptionalExit() override;
    void Step() override;
    VAddr GetTlsAddress() const override;
    void SetTlsAddress(VAddr address) override;
    void SetTPIDR_EL0(u64 value) override;
    u64 GetTPIDR_EL0() const override;
    void ChangeProcessorID(std::size_t new_core_id) override;

    bool IsInThumbMode() const {
        return (GetPSTATE() & 0x20) != 0;
    }

    void SaveContext(ThreadContext32& ctx) override;
    void SaveContext(ThreadContext64& ctx) override {}
    void LoadContext(const ThreadContext32& ctx) override;
    void LoadContext(const ThreadContext64& ctx) override {}

    void PrepareReschedule() override;
    void ClearExclusiveState() override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(VAddr addr, std::size_t size) override;
    void PageTableChanged(Common::PageTable& new_page_table,
                          std::size_t new_address_space_size_in_bits) override;

private:
    std::shared_ptr<Dynarmic::A32::Jit> MakeJit(Common::PageTable& page_table,
                                                std::size_t address_space_bits) const;

    using JitCacheKey = std::pair<Common::PageTable*, std::size_t>;
    using JitCacheType =
        std::unordered_map<JitCacheKey, std::shared_ptr<Dynarmic::A32::Jit>, Common::PairHash>;

    friend class DynarmicCallbacks32;
    friend class DynarmicCP15;

    std::unique_ptr<DynarmicCallbacks32> cb;
    JitCacheType jit_cache;
    std::shared_ptr<Dynarmic::A32::Jit> jit;
    std::shared_ptr<DynarmicCP15> cp15;
    std::size_t core_index;
    DynarmicExclusiveMonitor& exclusive_monitor;
};

} // namespace Core
