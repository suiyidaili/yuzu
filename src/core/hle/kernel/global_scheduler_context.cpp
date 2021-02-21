// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <mutex>

#include "common/assert.h"
#include "core/core.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

GlobalSchedulerContext::GlobalSchedulerContext(KernelCore& kernel)
    : kernel{kernel}, scheduler_lock{kernel} {}

GlobalSchedulerContext::~GlobalSchedulerContext() = default;

void GlobalSchedulerContext::AddThread(std::shared_ptr<KThread> thread) {
    std::scoped_lock lock{global_list_guard};
    thread_list.push_back(std::move(thread));
}

void GlobalSchedulerContext::RemoveThread(std::shared_ptr<KThread> thread) {
    std::scoped_lock lock{global_list_guard};
    thread_list.erase(std::remove(thread_list.begin(), thread_list.end(), thread),
                      thread_list.end());
}

void GlobalSchedulerContext::PreemptThreads() {
    // The priority levels at which the global scheduler preempts threads every 10 ms. They are
    // ordered from Core 0 to Core 3.
    static constexpr std::array<u32, Core::Hardware::NUM_CPU_CORES> preemption_priorities{
        59,
        59,
        59,
        63,
    };

    ASSERT(IsLocked());
    for (u32 core_id = 0; core_id < Core::Hardware::NUM_CPU_CORES; core_id++) {
        const u32 priority = preemption_priorities[core_id];
        kernel.Scheduler(core_id).RotateScheduledQueue(core_id, priority);
    }
}

bool GlobalSchedulerContext::IsLocked() const {
    return scheduler_lock.IsLockedByCurrentThread();
}

} // namespace Kernel
