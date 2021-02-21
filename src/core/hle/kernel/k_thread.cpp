// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <optional>
#include <vector>

#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/fiber.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/thread_queue_list.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/k_condition_variable.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory/memory_layout.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/kernel/time_manager.h"
#include "core/hle/result.h"
#include "core/memory.h"

#ifdef ARCHITECTURE_x86_64
#include "core/arm/dynarmic/arm_dynarmic_32.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#endif

namespace {
static void ResetThreadContext32(Core::ARM_Interface::ThreadContext32& context, u32 stack_top,
                                 u32 entry_point, u32 arg) {
    context = {};
    context.cpu_registers[0] = arg;
    context.cpu_registers[15] = entry_point;
    context.cpu_registers[13] = stack_top;
}

static void ResetThreadContext64(Core::ARM_Interface::ThreadContext64& context, VAddr stack_top,
                                 VAddr entry_point, u64 arg) {
    context = {};
    context.cpu_registers[0] = arg;
    context.pc = entry_point;
    context.sp = stack_top;
    // TODO(merry): Perform a hardware test to determine the below value.
    context.fpcr = 0;
}
} // namespace

namespace Kernel {

KThread::KThread(KernelCore& kernel)
    : KSynchronizationObject{kernel}, activity_pause_lock{kernel} {}
KThread::~KThread() = default;

ResultCode KThread::Initialize(KThreadFunction func, uintptr_t arg, VAddr user_stack_top, s32 prio,
                               s32 virt_core, Process* owner, ThreadType type) {
    // Assert parameters are valid.
    ASSERT((type == ThreadType::Main) ||
           (Svc::HighestThreadPriority <= prio && prio <= Svc::LowestThreadPriority));
    ASSERT((owner != nullptr) || (type != ThreadType::User));
    ASSERT(0 <= virt_core && virt_core < static_cast<s32>(Common::BitSize<u64>()));

    // Convert the virtual core to a physical core.
    const s32 phys_core = Core::Hardware::VirtualToPhysicalCoreMap[virt_core];
    ASSERT(0 <= phys_core && phys_core < static_cast<s32>(Core::Hardware::NUM_CPU_CORES));

    // First, clear the TLS address.
    tls_address = {};

    // Next, assert things based on the type.
    switch (type) {
    case ThreadType::Main:
        ASSERT(arg == 0);
        [[fallthrough]];
    case ThreadType::HighPriority:
        [[fallthrough]];
    case ThreadType::User:
        ASSERT(((owner == nullptr) ||
                (owner->GetCoreMask() | (1ULL << virt_core)) == owner->GetCoreMask()));
        ASSERT(((owner == nullptr) ||
                (owner->GetPriorityMask() | (1ULL << prio)) == owner->GetPriorityMask()));
        break;
    case ThreadType::Kernel:
        UNIMPLEMENTED();
        break;
    default:
        UNREACHABLE_MSG("KThread::Initialize: Unknown ThreadType {}", static_cast<u32>(type));
        break;
    }
    thread_type_for_debugging = type;

    // Set the ideal core ID and affinity mask.
    virtual_ideal_core_id = virt_core;
    physical_ideal_core_id = phys_core;
    virtual_affinity_mask = 1ULL << virt_core;
    physical_affinity_mask.SetAffinity(phys_core, true);

    // Set the thread state.
    thread_state = (type == ThreadType::Main) ? ThreadState::Runnable : ThreadState::Initialized;

    // Set TLS address.
    tls_address = 0;

    // Set parent and condvar tree.
    parent = nullptr;
    condvar_tree = nullptr;

    // Set sync booleans.
    signaled = false;
    termination_requested = false;
    wait_cancelled = false;
    cancellable = false;

    // Set core ID and wait result.
    core_id = phys_core;
    wait_result = ResultNoSynchronizationObject;

    // Set priorities.
    priority = prio;
    base_priority = prio;

    // Set sync object and waiting lock to null.
    synced_object = nullptr;

    // Initialize sleeping queue.
    sleeping_queue = nullptr;

    // Set suspend flags.
    suspend_request_flags = 0;
    suspend_allowed_flags = static_cast<u32>(ThreadState::SuspendFlagMask);

    // We're neither debug attached, nor are we nesting our priority inheritance.
    debug_attached = false;
    priority_inheritance_count = 0;

    // We haven't been scheduled, and we have done no light IPC.
    schedule_count = -1;
    last_scheduled_tick = 0;
    light_ipc_data = nullptr;

    // We're not waiting for a lock, and we haven't disabled migration.
    lock_owner = nullptr;
    num_core_migration_disables = 0;

    // We have no waiters, but we do have an entrypoint.
    num_kernel_waiters = 0;

    // Set our current core id.
    current_core_id = phys_core;

    // We haven't released our resource limit hint, and we've spent no time on the cpu.
    resource_limit_release_hint = false;
    cpu_time = 0;

    // Clear our stack parameters.
    std::memset(static_cast<void*>(std::addressof(GetStackParameters())), 0,
                sizeof(StackParameters));

    // Setup the TLS, if needed.
    if (type == ThreadType::User) {
        tls_address = owner->CreateTLSRegion();
    }

    // Set parent, if relevant.
    if (owner != nullptr) {
        parent = owner;
        parent->IncrementThreadCount();
    }

    // Initialize thread context.
    ResetThreadContext64(thread_context_64, user_stack_top, func, arg);
    ResetThreadContext32(thread_context_32, static_cast<u32>(user_stack_top),
                         static_cast<u32>(func), static_cast<u32>(arg));

    // Setup the stack parameters.
    StackParameters& sp = GetStackParameters();
    sp.cur_thread = this;
    sp.disable_count = 1;
    SetInExceptionHandler();

    // Set thread ID.
    thread_id = kernel.CreateNewThreadID();

    // We initialized!
    initialized = true;

    // Register ourselves with our parent process.
    if (parent != nullptr) {
        parent->RegisterThread(this);
        if (parent->IsSuspended()) {
            RequestSuspend(SuspendType::Process);
        }
    }

    return RESULT_SUCCESS;
}

ResultCode KThread::InitializeThread(KThread* thread, KThreadFunction func, uintptr_t arg,
                                     VAddr user_stack_top, s32 prio, s32 core, Process* owner,
                                     ThreadType type) {
    // Initialize the thread.
    R_TRY(thread->Initialize(func, arg, user_stack_top, prio, core, owner, type));

    return RESULT_SUCCESS;
}

void KThread::Finalize() {
    // If the thread has an owner process, unregister it.
    if (parent != nullptr) {
        parent->UnregisterThread(this);
    }

    // If the thread has a local region, delete it.
    if (tls_address != 0) {
        parent->FreeTLSRegion(tls_address);
    }

    // Release any waiters.
    {
        ASSERT(lock_owner == nullptr);
        KScopedSchedulerLock sl{kernel};

        auto it = waiter_list.begin();
        while (it != waiter_list.end()) {
            // The thread shouldn't be a kernel waiter.
            it->SetLockOwner(nullptr);
            it->SetSyncedObject(nullptr, ResultInvalidState);
            it->Wakeup();
            it = waiter_list.erase(it);
        }
    }

    // Decrement the parent process's thread count.
    if (parent != nullptr) {
        parent->DecrementThreadCount();
        parent->GetResourceLimit()->Release(LimitableResource::Threads, 1);
    }
}

bool KThread::IsSignaled() const {
    return signaled;
}

void KThread::Wakeup() {
    KScopedSchedulerLock sl{kernel};

    if (GetState() == ThreadState::Waiting) {
        if (sleeping_queue != nullptr) {
            sleeping_queue->WakeupThread(this);
        } else {
            SetState(ThreadState::Runnable);
        }
    }
}

void KThread::StartTermination() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Release user exception and unpin, if relevant.
    if (parent != nullptr) {
        parent->ReleaseUserException(this);
        if (parent->GetPinnedThread(GetCurrentCoreId(kernel)) == this) {
            parent->UnpinCurrentThread();
        }
    }

    // Set state to terminated.
    SetState(ThreadState::Terminated);

    // Clear the thread's status as running in parent.
    if (parent != nullptr) {
        parent->ClearRunningThread(this);
    }

    // Signal.
    signaled = true;
    NotifyAvailable();

    // Clear previous thread in KScheduler.
    KScheduler::ClearPreviousThread(kernel, this);

    // Register terminated dpc flag.
    RegisterDpc(DpcFlag::Terminated);
}

void KThread::Pin() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Set ourselves as pinned.
    GetStackParameters().is_pinned = true;

    // Disable core migration.
    ASSERT(num_core_migration_disables == 0);
    {
        ++num_core_migration_disables;

        // Save our ideal state to restore when we're unpinned.
        original_physical_ideal_core_id = physical_ideal_core_id;
        original_physical_affinity_mask = physical_affinity_mask;

        // Bind ourselves to this core.
        const s32 active_core = GetActiveCore();
        const s32 current_core = GetCurrentCoreId(kernel);

        SetActiveCore(current_core);
        physical_ideal_core_id = current_core;
        physical_affinity_mask.SetAffinityMask(1ULL << current_core);

        if (active_core != current_core || physical_affinity_mask.GetAffinityMask() !=
                                               original_physical_affinity_mask.GetAffinityMask()) {
            KScheduler::OnThreadAffinityMaskChanged(kernel, this, original_physical_affinity_mask,
                                                    active_core);
        }
    }

    // Disallow performing thread suspension.
    {
        // Update our allow flags.
        suspend_allowed_flags &= ~(1 << (static_cast<u32>(SuspendType::Thread) +
                                         static_cast<u32>(ThreadState::SuspendShift)));

        // Update our state.
        const ThreadState old_state = thread_state;
        thread_state = static_cast<ThreadState>(GetSuspendFlags() |
                                                static_cast<u32>(old_state & ThreadState::Mask));
        if (thread_state != old_state) {
            KScheduler::OnThreadStateChanged(kernel, this, old_state);
        }
    }

    // TODO(bunnei): Update our SVC access permissions.
    ASSERT(parent != nullptr);
}

void KThread::Unpin() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Set ourselves as unpinned.
    GetStackParameters().is_pinned = false;

    // Enable core migration.
    ASSERT(num_core_migration_disables == 1);
    {
        num_core_migration_disables--;

        // Restore our original state.
        const KAffinityMask old_mask = physical_affinity_mask;

        physical_ideal_core_id = original_physical_ideal_core_id;
        physical_affinity_mask = original_physical_affinity_mask;

        if (physical_affinity_mask.GetAffinityMask() != old_mask.GetAffinityMask()) {
            const s32 active_core = GetActiveCore();

            if (!physical_affinity_mask.GetAffinity(active_core)) {
                if (physical_ideal_core_id >= 0) {
                    SetActiveCore(physical_ideal_core_id);
                } else {
                    SetActiveCore(static_cast<s32>(
                        Common::BitSize<u64>() - 1 -
                        std::countl_zero(physical_affinity_mask.GetAffinityMask())));
                }
            }
            KScheduler::OnThreadAffinityMaskChanged(kernel, this, old_mask, active_core);
        }
    }

    // Allow performing thread suspension (if termination hasn't been requested).
    {
        // Update our allow flags.
        if (!IsTerminationRequested()) {
            suspend_allowed_flags |= (1 << (static_cast<u32>(SuspendType::Thread) +
                                            static_cast<u32>(ThreadState::SuspendShift)));
        }

        // Update our state.
        const ThreadState old_state = thread_state;
        thread_state = static_cast<ThreadState>(GetSuspendFlags() |
                                                static_cast<u32>(old_state & ThreadState::Mask));
        if (thread_state != old_state) {
            KScheduler::OnThreadStateChanged(kernel, this, old_state);
        }
    }

    // TODO(bunnei): Update our SVC access permissions.
    ASSERT(parent != nullptr);

    // Resume any threads that began waiting on us while we were pinned.
    for (auto it = pinned_waiter_list.begin(); it != pinned_waiter_list.end(); ++it) {
        if (it->GetState() == ThreadState::Waiting) {
            it->SetState(ThreadState::Runnable);
        }
    }
}

ResultCode KThread::GetCoreMask(s32* out_ideal_core, u64* out_affinity_mask) {
    KScopedSchedulerLock sl{kernel};

    // Get the virtual mask.
    *out_ideal_core = virtual_ideal_core_id;
    *out_affinity_mask = virtual_affinity_mask;

    return RESULT_SUCCESS;
}

ResultCode KThread::GetPhysicalCoreMask(s32* out_ideal_core, u64* out_affinity_mask) {
    KScopedSchedulerLock sl{kernel};
    ASSERT(num_core_migration_disables >= 0);

    // Select between core mask and original core mask.
    if (num_core_migration_disables == 0) {
        *out_ideal_core = physical_ideal_core_id;
        *out_affinity_mask = physical_affinity_mask.GetAffinityMask();
    } else {
        *out_ideal_core = original_physical_ideal_core_id;
        *out_affinity_mask = original_physical_affinity_mask.GetAffinityMask();
    }

    return RESULT_SUCCESS;
}

ResultCode KThread::SetCoreMask(s32 core_id, u64 v_affinity_mask) {
    ASSERT(parent != nullptr);
    ASSERT(v_affinity_mask != 0);
    KScopedLightLock lk{activity_pause_lock};

    // Set the core mask.
    u64 p_affinity_mask = 0;
    {
        KScopedSchedulerLock sl{kernel};
        ASSERT(num_core_migration_disables >= 0);

        // If the core id is no-update magic, preserve the ideal core id.
        if (core_id == Svc::IdealCoreNoUpdate) {
            core_id = virtual_ideal_core_id;
            R_UNLESS(((1ULL << core_id) & v_affinity_mask) != 0, ResultInvalidCombination);
        }

        // Set the virtual core/affinity mask.
        virtual_ideal_core_id = core_id;
        virtual_affinity_mask = v_affinity_mask;

        // Translate the virtual core to a physical core.
        if (core_id >= 0) {
            core_id = Core::Hardware::VirtualToPhysicalCoreMap[core_id];
        }

        // Translate the virtual affinity mask to a physical one.
        while (v_affinity_mask != 0) {
            const u64 next = std::countr_zero(v_affinity_mask);
            v_affinity_mask &= ~(1ULL << next);
            p_affinity_mask |= (1ULL << Core::Hardware::VirtualToPhysicalCoreMap[next]);
        }

        // If we haven't disabled migration, perform an affinity change.
        if (num_core_migration_disables == 0) {
            const KAffinityMask old_mask = physical_affinity_mask;

            // Set our new ideals.
            physical_ideal_core_id = core_id;
            physical_affinity_mask.SetAffinityMask(p_affinity_mask);

            if (physical_affinity_mask.GetAffinityMask() != old_mask.GetAffinityMask()) {
                const s32 active_core = GetActiveCore();

                if (active_core >= 0 && !physical_affinity_mask.GetAffinity(active_core)) {
                    const s32 new_core = static_cast<s32>(
                        physical_ideal_core_id >= 0
                            ? physical_ideal_core_id
                            : Common::BitSize<u64>() - 1 -
                                  std::countl_zero(physical_affinity_mask.GetAffinityMask()));
                    SetActiveCore(new_core);
                }
                KScheduler::OnThreadAffinityMaskChanged(kernel, this, old_mask, active_core);
            }
        } else {
            // Otherwise, we edit the original affinity for restoration later.
            original_physical_ideal_core_id = core_id;
            original_physical_affinity_mask.SetAffinityMask(p_affinity_mask);
        }
    }

    // Update the pinned waiter list.
    {
        bool retry_update{};
        bool thread_is_pinned{};
        do {
            // Lock the scheduler.
            KScopedSchedulerLock sl{kernel};

            // Don't do any further management if our termination has been requested.
            R_SUCCEED_IF(IsTerminationRequested());

            // By default, we won't need to retry.
            retry_update = false;

            // Check if the thread is currently running.
            bool thread_is_current{};
            s32 thread_core;
            for (thread_core = 0; thread_core < static_cast<s32>(Core::Hardware::NUM_CPU_CORES);
                 ++thread_core) {
                if (kernel.Scheduler(thread_core).GetCurrentThread() == this) {
                    thread_is_current = true;
                    break;
                }
            }

            // If the thread is currently running, check whether it's no longer allowed under the
            // new mask.
            if (thread_is_current && ((1ULL << thread_core) & p_affinity_mask) == 0) {
                // If the thread is pinned, we want to wait until it's not pinned.
                if (GetStackParameters().is_pinned) {
                    // Verify that the current thread isn't terminating.
                    R_UNLESS(!GetCurrentThread(kernel).IsTerminationRequested(),
                             ResultTerminationRequested);

                    // Note that the thread was pinned.
                    thread_is_pinned = true;

                    // Wait until the thread isn't pinned any more.
                    pinned_waiter_list.push_back(GetCurrentThread(kernel));
                    GetCurrentThread(kernel).SetState(ThreadState::Waiting);
                } else {
                    // If the thread isn't pinned, release the scheduler lock and retry until it's
                    // not current.
                    retry_update = true;
                }
            }
        } while (retry_update);

        // If the thread was pinned, it no longer is, and we should remove the current thread from
        // our waiter list.
        if (thread_is_pinned) {
            // Lock the scheduler.
            KScopedSchedulerLock sl{kernel};

            // Remove from the list.
            pinned_waiter_list.erase(pinned_waiter_list.iterator_to(GetCurrentThread(kernel)));
        }
    }

    return RESULT_SUCCESS;
}

void KThread::SetBasePriority(s32 value) {
    ASSERT(Svc::HighestThreadPriority <= value && value <= Svc::LowestThreadPriority);

    KScopedSchedulerLock sl{kernel};

    // Change our base priority.
    base_priority = value;

    // Perform a priority restoration.
    RestorePriority(kernel, this);
}

void KThread::RequestSuspend(SuspendType type) {
    KScopedSchedulerLock sl{kernel};

    // Note the request in our flags.
    suspend_request_flags |=
        (1u << (static_cast<u32>(ThreadState::SuspendShift) + static_cast<u32>(type)));

    // Try to perform the suspend.
    TrySuspend();
}

void KThread::Resume(SuspendType type) {
    KScopedSchedulerLock sl{kernel};

    // Clear the request in our flags.
    suspend_request_flags &=
        ~(1u << (static_cast<u32>(ThreadState::SuspendShift) + static_cast<u32>(type)));

    // Update our state.
    const ThreadState old_state = thread_state;
    thread_state = static_cast<ThreadState>(GetSuspendFlags() |
                                            static_cast<u32>(old_state & ThreadState::Mask));
    if (thread_state != old_state) {
        KScheduler::OnThreadStateChanged(kernel, this, old_state);
    }
}

void KThread::WaitCancel() {
    KScopedSchedulerLock sl{kernel};

    // Check if we're waiting and cancellable.
    if (GetState() == ThreadState::Waiting && cancellable) {
        if (sleeping_queue != nullptr) {
            sleeping_queue->WakeupThread(this);
            wait_cancelled = true;
        } else {
            SetSyncedObject(nullptr, ResultCancelled);
            SetState(ThreadState::Runnable);
            wait_cancelled = false;
        }
    } else {
        // Otherwise, note that we cancelled a wait.
        wait_cancelled = true;
    }
}

void KThread::TrySuspend() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());
    ASSERT(IsSuspendRequested());

    // Ensure that we have no waiters.
    if (GetNumKernelWaiters() > 0) {
        return;
    }
    ASSERT(GetNumKernelWaiters() == 0);

    // Perform the suspend.
    Suspend();
}

void KThread::Suspend() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());
    ASSERT(IsSuspendRequested());

    // Set our suspend flags in state.
    const auto old_state = thread_state;
    thread_state = static_cast<ThreadState>(GetSuspendFlags()) | (old_state & ThreadState::Mask);

    // Note the state change in scheduler.
    KScheduler::OnThreadStateChanged(kernel, this, old_state);
}

void KThread::Continue() {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Clear our suspend flags in state.
    const auto old_state = thread_state;
    thread_state = old_state & ThreadState::Mask;

    // Note the state change in scheduler.
    KScheduler::OnThreadStateChanged(kernel, this, old_state);
}

ResultCode KThread::SetActivity(Svc::ThreadActivity activity) {
    // Lock ourselves.
    KScopedLightLock lk(activity_pause_lock);

    // Set the activity.
    {
        // Lock the scheduler.
        KScopedSchedulerLock sl{kernel};

        // Verify our state.
        const auto cur_state = GetState();
        R_UNLESS((cur_state == ThreadState::Waiting || cur_state == ThreadState::Runnable),
                 ResultInvalidState);

        // Either pause or resume.
        if (activity == Svc::ThreadActivity::Paused) {
            // Verify that we're not suspended.
            R_UNLESS(!IsSuspendRequested(SuspendType::Thread), ResultInvalidState);

            // Suspend.
            RequestSuspend(SuspendType::Thread);
        } else {
            ASSERT(activity == Svc::ThreadActivity::Runnable);

            // Verify that we're suspended.
            R_UNLESS(IsSuspendRequested(SuspendType::Thread), ResultInvalidState);

            // Resume.
            Resume(SuspendType::Thread);
        }
    }

    // If the thread is now paused, update the pinned waiter list.
    if (activity == Svc::ThreadActivity::Paused) {
        bool thread_is_pinned{};
        bool thread_is_current{};
        do {
            // Lock the scheduler.
            KScopedSchedulerLock sl{kernel};

            // Don't do any further management if our termination has been requested.
            R_SUCCEED_IF(IsTerminationRequested());

            // Check whether the thread is pinned.
            if (GetStackParameters().is_pinned) {
                // Verify that the current thread isn't terminating.
                R_UNLESS(!GetCurrentThread(kernel).IsTerminationRequested(),
                         ResultTerminationRequested);

                // Note that the thread was pinned and not current.
                thread_is_pinned = true;
                thread_is_current = false;

                // Wait until the thread isn't pinned any more.
                pinned_waiter_list.push_back(GetCurrentThread(kernel));
                GetCurrentThread(kernel).SetState(ThreadState::Waiting);
            } else {
                // Check if the thread is currently running.
                // If it is, we'll need to retry.
                thread_is_current = false;

                for (auto i = 0; i < static_cast<s32>(Core::Hardware::NUM_CPU_CORES); ++i) {
                    if (kernel.Scheduler(i).GetCurrentThread() == this) {
                        thread_is_current = true;
                        break;
                    }
                }
            }
        } while (thread_is_current);

        // If the thread was pinned, it no longer is, and we should remove the current thread from
        // our waiter list.
        if (thread_is_pinned) {
            // Lock the scheduler.
            KScopedSchedulerLock sl{kernel};

            // Remove from the list.
            pinned_waiter_list.erase(pinned_waiter_list.iterator_to(GetCurrentThread(kernel)));
        }
    }

    return RESULT_SUCCESS;
}

ResultCode KThread::GetThreadContext3(std::vector<u8>& out) {
    // Lock ourselves.
    KScopedLightLock lk{activity_pause_lock};

    // Get the context.
    {
        // Lock the scheduler.
        KScopedSchedulerLock sl{kernel};

        // Verify that we're suspended.
        R_UNLESS(IsSuspendRequested(SuspendType::Thread), ResultInvalidState);

        // If we're not terminating, get the thread's user context.
        if (!IsTerminationRequested()) {
            if (parent->Is64BitProcess()) {
                // Mask away mode bits, interrupt bits, IL bit, and other reserved bits.
                auto context = GetContext64();
                context.pstate &= 0xFF0FFE20;

                out.resize(sizeof(context));
                std::memcpy(out.data(), &context, sizeof(context));
            } else {
                // Mask away mode bits, interrupt bits, IL bit, and other reserved bits.
                auto context = GetContext32();
                context.cpsr &= 0xFF0FFE20;

                out.resize(sizeof(context));
                std::memcpy(out.data(), &context, sizeof(context));
            }
        }
    }

    return RESULT_SUCCESS;
}

void KThread::AddWaiterImpl(KThread* thread) {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Find the right spot to insert the waiter.
    auto it = waiter_list.begin();
    while (it != waiter_list.end()) {
        if (it->GetPriority() > thread->GetPriority()) {
            break;
        }
        it++;
    }

    // Keep track of how many kernel waiters we have.
    if (Memory::IsKernelAddressKey(thread->GetAddressKey())) {
        ASSERT((num_kernel_waiters++) >= 0);
    }

    // Insert the waiter.
    waiter_list.insert(it, *thread);
    thread->SetLockOwner(this);
}

void KThread::RemoveWaiterImpl(KThread* thread) {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    // Keep track of how many kernel waiters we have.
    if (Memory::IsKernelAddressKey(thread->GetAddressKey())) {
        ASSERT((num_kernel_waiters--) > 0);
    }

    // Remove the waiter.
    waiter_list.erase(waiter_list.iterator_to(*thread));
    thread->SetLockOwner(nullptr);
}

void KThread::RestorePriority(KernelCore& kernel, KThread* thread) {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    while (true) {
        // We want to inherit priority where possible.
        s32 new_priority = thread->GetBasePriority();
        if (thread->HasWaiters()) {
            new_priority = std::min(new_priority, thread->waiter_list.front().GetPriority());
        }

        // If the priority we would inherit is not different from ours, don't do anything.
        if (new_priority == thread->GetPriority()) {
            return;
        }

        // Ensure we don't violate condition variable red black tree invariants.
        if (auto* cv_tree = thread->GetConditionVariableTree(); cv_tree != nullptr) {
            BeforeUpdatePriority(kernel, cv_tree, thread);
        }

        // Change the priority.
        const s32 old_priority = thread->GetPriority();
        thread->SetPriority(new_priority);

        // Restore the condition variable, if relevant.
        if (auto* cv_tree = thread->GetConditionVariableTree(); cv_tree != nullptr) {
            AfterUpdatePriority(kernel, cv_tree, thread);
        }

        // Update the scheduler.
        KScheduler::OnThreadPriorityChanged(kernel, thread, old_priority);

        // Keep the lock owner up to date.
        KThread* lock_owner = thread->GetLockOwner();
        if (lock_owner == nullptr) {
            return;
        }

        // Update the thread in the lock owner's sorted list, and continue inheriting.
        lock_owner->RemoveWaiterImpl(thread);
        lock_owner->AddWaiterImpl(thread);
        thread = lock_owner;
    }
}

void KThread::AddWaiter(KThread* thread) {
    AddWaiterImpl(thread);
    RestorePriority(kernel, this);
}

void KThread::RemoveWaiter(KThread* thread) {
    RemoveWaiterImpl(thread);
    RestorePriority(kernel, this);
}

KThread* KThread::RemoveWaiterByKey(s32* out_num_waiters, VAddr key) {
    ASSERT(kernel.GlobalSchedulerContext().IsLocked());

    s32 num_waiters{};
    KThread* next_lock_owner{};
    auto it = waiter_list.begin();
    while (it != waiter_list.end()) {
        if (it->GetAddressKey() == key) {
            KThread* thread = std::addressof(*it);

            // Keep track of how many kernel waiters we have.
            if (Memory::IsKernelAddressKey(thread->GetAddressKey())) {
                ASSERT((num_kernel_waiters--) > 0);
            }
            it = waiter_list.erase(it);

            // Update the next lock owner.
            if (next_lock_owner == nullptr) {
                next_lock_owner = thread;
                next_lock_owner->SetLockOwner(nullptr);
            } else {
                next_lock_owner->AddWaiterImpl(thread);
            }
            num_waiters++;
        } else {
            it++;
        }
    }

    // Do priority updates, if we have a next owner.
    if (next_lock_owner) {
        RestorePriority(kernel, this);
        RestorePriority(kernel, next_lock_owner);
    }

    // Return output.
    *out_num_waiters = num_waiters;
    return next_lock_owner;
}

ResultCode KThread::Run() {
    while (true) {
        KScopedSchedulerLock lk{kernel};

        // If either this thread or the current thread are requesting termination, note it.
        R_UNLESS(!IsTerminationRequested(), ResultTerminationRequested);
        R_UNLESS(!GetCurrentThread(kernel).IsTerminationRequested(), ResultTerminationRequested);

        // Ensure our thread state is correct.
        R_UNLESS(GetState() == ThreadState::Initialized, ResultInvalidState);

        // If the current thread has been asked to suspend, suspend it and retry.
        if (GetCurrentThread(kernel).IsSuspended()) {
            GetCurrentThread(kernel).Suspend();
            continue;
        }

        // If we're not a kernel thread and we've been asked to suspend, suspend ourselves.
        if (IsUserThread() && IsSuspended()) {
            Suspend();
        }

        // Set our state and finish.
        SetState(ThreadState::Runnable);
        return RESULT_SUCCESS;
    }
}

void KThread::Exit() {
    ASSERT(this == GetCurrentThreadPointer(kernel));

    // Release the thread resource hint from parent.
    if (parent != nullptr) {
        // TODO(bunnei): Hint that the resource is about to be released.
        resource_limit_release_hint = true;
    }

    // Perform termination.
    {
        KScopedSchedulerLock sl{kernel};

        // Disallow all suspension.
        suspend_allowed_flags = 0;

        // Start termination.
        StartTermination();
    }
}

ResultCode KThread::Sleep(s64 timeout) {
    ASSERT(!kernel.GlobalSchedulerContext().IsLocked());
    ASSERT(this == GetCurrentThreadPointer(kernel));
    ASSERT(timeout > 0);

    {
        // Setup the scheduling lock and sleep.
        KScopedSchedulerLockAndSleep slp{kernel, this, timeout};

        // Check if the thread should terminate.
        if (IsTerminationRequested()) {
            slp.CancelSleep();
            return ResultTerminationRequested;
        }

        // Mark the thread as waiting.
        SetState(ThreadState::Waiting);
        SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::Sleep);
    }

    // The lock/sleep is done.

    // Cancel the timer.
    kernel.TimeManager().UnscheduleTimeEvent(this);

    return RESULT_SUCCESS;
}

void KThread::SetState(ThreadState state) {
    KScopedSchedulerLock sl{kernel};

    // Clear debugging state
    SetMutexWaitAddressForDebugging({});
    SetWaitReasonForDebugging({});

    const ThreadState old_state = thread_state;
    thread_state =
        static_cast<ThreadState>((old_state & ~ThreadState::Mask) | (state & ThreadState::Mask));
    if (thread_state != old_state) {
        KScheduler::OnThreadStateChanged(kernel, this, old_state);
    }
}

std::shared_ptr<Common::Fiber>& KThread::GetHostContext() {
    return host_context;
}

ResultVal<std::shared_ptr<KThread>> KThread::Create(Core::System& system, ThreadType type_flags,
                                                    std::string name, VAddr entry_point,
                                                    u32 priority, u64 arg, s32 processor_id,
                                                    VAddr stack_top, Process* owner_process) {
    std::function<void(void*)> init_func = Core::CpuManager::GetGuestThreadStartFunc();
    void* init_func_parameter = system.GetCpuManager().GetStartFuncParamater();
    return Create(system, type_flags, name, entry_point, priority, arg, processor_id, stack_top,
                  owner_process, std::move(init_func), init_func_parameter);
}

ResultVal<std::shared_ptr<KThread>> KThread::Create(Core::System& system, ThreadType type_flags,
                                                    std::string name, VAddr entry_point,
                                                    u32 priority, u64 arg, s32 processor_id,
                                                    VAddr stack_top, Process* owner_process,
                                                    std::function<void(void*)>&& thread_start_func,
                                                    void* thread_start_parameter) {
    auto& kernel = system.Kernel();

    std::shared_ptr<KThread> thread = std::make_shared<KThread>(kernel);

    if (const auto result =
            thread->InitializeThread(thread.get(), entry_point, arg, stack_top, priority,
                                     processor_id, owner_process, type_flags);
        result.IsError()) {
        return result;
    }

    thread->name = name;

    auto& scheduler = kernel.GlobalSchedulerContext();
    scheduler.AddThread(thread);

    thread->host_context =
        std::make_shared<Common::Fiber>(std::move(thread_start_func), thread_start_parameter);

    return MakeResult<std::shared_ptr<KThread>>(std::move(thread));
}

KThread* GetCurrentThreadPointer(KernelCore& kernel) {
    return kernel.GetCurrentEmuThread();
}

KThread& GetCurrentThread(KernelCore& kernel) {
    return *GetCurrentThreadPointer(kernel);
}

s32 GetCurrentCoreId(KernelCore& kernel) {
    return GetCurrentThread(kernel).GetCurrentCore();
}

} // namespace Kernel
