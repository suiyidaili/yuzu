// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/time_manager.h"

namespace Kernel {

TimeManager::TimeManager(Core::System& system_) : system{system_} {
    time_manager_event_type = Core::Timing::CreateEvent(
        "Kernel::TimeManagerCallback",
        [this](std::uintptr_t thread_handle, std::chrono::nanoseconds) {
            std::shared_ptr<KThread> thread;
            {
                std::lock_guard lock{mutex};
                thread = SharedFrom<KThread>(reinterpret_cast<KThread*>(thread_handle));
            }
            thread->Wakeup();
        });
}

void TimeManager::ScheduleTimeEvent(KThread* thread, s64 nanoseconds) {
    std::lock_guard lock{mutex};
    if (nanoseconds > 0) {
        ASSERT(thread);
        ASSERT(thread->GetState() != ThreadState::Runnable);
        system.CoreTiming().ScheduleEvent(std::chrono::nanoseconds{nanoseconds},
                                          time_manager_event_type,
                                          reinterpret_cast<uintptr_t>(thread));
    }
}

void TimeManager::UnscheduleTimeEvent(KThread* thread) {
    std::lock_guard lock{mutex};
    system.CoreTiming().UnscheduleEvent(time_manager_event_type,
                                        reinterpret_cast<uintptr_t>(thread));
}

} // namespace Kernel
