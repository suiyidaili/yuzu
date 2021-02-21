// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <memory>
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/memory.h"

namespace Core {

DynarmicExclusiveMonitor::DynarmicExclusiveMonitor(Memory::Memory& memory, std::size_t core_count)
    : monitor(core_count), memory{memory} {}

DynarmicExclusiveMonitor::~DynarmicExclusiveMonitor() = default;

u8 DynarmicExclusiveMonitor::ExclusiveRead8(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u8>(core_index, addr, [&]() -> u8 { return memory.Read8(addr); });
}

u16 DynarmicExclusiveMonitor::ExclusiveRead16(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u16>(core_index, addr, [&]() -> u16 { return memory.Read16(addr); });
}

u32 DynarmicExclusiveMonitor::ExclusiveRead32(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u32>(core_index, addr, [&]() -> u32 { return memory.Read32(addr); });
}

u64 DynarmicExclusiveMonitor::ExclusiveRead64(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u64>(core_index, addr, [&]() -> u64 { return memory.Read64(addr); });
}

u128 DynarmicExclusiveMonitor::ExclusiveRead128(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u128>(core_index, addr, [&]() -> u128 {
        u128 result;
        result[0] = memory.Read64(addr);
        result[1] = memory.Read64(addr + 8);
        return result;
    });
}

void DynarmicExclusiveMonitor::ClearExclusive() {
    monitor.Clear();
}

bool DynarmicExclusiveMonitor::ExclusiveWrite8(std::size_t core_index, VAddr vaddr, u8 value) {
    return monitor.DoExclusiveOperation<u8>(core_index, vaddr, [&](u8 expected) -> bool {
        return memory.WriteExclusive8(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite16(std::size_t core_index, VAddr vaddr, u16 value) {
    return monitor.DoExclusiveOperation<u16>(core_index, vaddr, [&](u16 expected) -> bool {
        return memory.WriteExclusive16(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite32(std::size_t core_index, VAddr vaddr, u32 value) {
    return monitor.DoExclusiveOperation<u32>(core_index, vaddr, [&](u32 expected) -> bool {
        return memory.WriteExclusive32(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite64(std::size_t core_index, VAddr vaddr, u64 value) {
    return monitor.DoExclusiveOperation<u64>(core_index, vaddr, [&](u64 expected) -> bool {
        return memory.WriteExclusive64(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite128(std::size_t core_index, VAddr vaddr, u128 value) {
    return monitor.DoExclusiveOperation<u128>(core_index, vaddr, [&](u128 expected) -> bool {
        return memory.WriteExclusive128(vaddr, value, expected);
    });
}

} // namespace Core
