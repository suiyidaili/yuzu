// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/intrusive/list.hpp>

#include "common/common_types.h"
#include "common/intrusive_red_black_tree.h"
#include "common/spin_lock.h"
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/k_affinity_mask.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/svc_common.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/result.h"

namespace Common {
class Fiber;
}

namespace Core {
class ARM_Interface;
class System;
} // namespace Core

namespace Kernel {

class GlobalSchedulerContext;
class KernelCore;
class Process;
class KScheduler;
class KThreadQueue;

using KThreadFunction = VAddr;

enum class ThreadType : u32 {
    Main = 0,
    Kernel = 1,
    HighPriority = 2,
    User = 3,
};
DECLARE_ENUM_FLAG_OPERATORS(ThreadType);

enum class SuspendType : u32 {
    Process = 0,
    Thread = 1,
    Debug = 2,
    Backtrace = 3,
    Init = 4,

    Count,
};

enum class ThreadState : u16 {
    Initialized = 0,
    Waiting = 1,
    Runnable = 2,
    Terminated = 3,

    SuspendShift = 4,
    Mask = (1 << SuspendShift) - 1,

    ProcessSuspended = (1 << (0 + SuspendShift)),
    ThreadSuspended = (1 << (1 + SuspendShift)),
    DebugSuspended = (1 << (2 + SuspendShift)),
    BacktraceSuspended = (1 << (3 + SuspendShift)),
    InitSuspended = (1 << (4 + SuspendShift)),

    SuspendFlagMask = ((1 << 5) - 1) << SuspendShift,
};
DECLARE_ENUM_FLAG_OPERATORS(ThreadState);

enum class DpcFlag : u32 {
    Terminating = (1 << 0),
    Terminated = (1 << 1),
};

enum class ThreadWaitReasonForDebugging : u32 {
    None,            ///< Thread is not waiting
    Sleep,           ///< Thread is waiting due to a SleepThread SVC
    IPC,             ///< Thread is waiting for the reply from an IPC request
    Synchronization, ///< Thread is waiting due to a WaitSynchronization SVC
    ConditionVar,    ///< Thread is waiting due to a WaitProcessWideKey SVC
    Arbitration,     ///< Thread is waiting due to a SignalToAddress/WaitForAddress SVC
    Suspended,       ///< Thread is waiting due to process suspension
};

[[nodiscard]] KThread* GetCurrentThreadPointer(KernelCore& kernel);
[[nodiscard]] KThread& GetCurrentThread(KernelCore& kernel);
[[nodiscard]] s32 GetCurrentCoreId(KernelCore& kernel);

class KThread final : public KSynchronizationObject, public boost::intrusive::list_base_hook<> {
    friend class KScheduler;
    friend class Process;

public:
    static constexpr s32 DefaultThreadPriority = 44;
    static constexpr s32 IdleThreadPriority = Svc::LowestThreadPriority + 1;

    explicit KThread(KernelCore& kernel);
    ~KThread() override;

public:
    using ThreadContext32 = Core::ARM_Interface::ThreadContext32;
    using ThreadContext64 = Core::ARM_Interface::ThreadContext64;
    using WaiterList = boost::intrusive::list<KThread>;

    /**
     * Creates and returns a new thread. The new thread is immediately scheduled
     * @param system The instance of the whole system
     * @param name The friendly name desired for the thread
     * @param entry_point The address at which the thread should start execution
     * @param priority The thread's priority
     * @param arg User data to pass to the thread
     * @param processor_id The ID(s) of the processors on which the thread is desired to be run
     * @param stack_top The address of the thread's stack top
     * @param owner_process The parent process for the thread, if null, it's a kernel thread
     * @return A shared pointer to the newly created thread
     */
    [[nodiscard]] static ResultVal<std::shared_ptr<KThread>> Create(
        Core::System& system, ThreadType type_flags, std::string name, VAddr entry_point,
        u32 priority, u64 arg, s32 processor_id, VAddr stack_top, Process* owner_process);

    /**
     * Creates and returns a new thread. The new thread is immediately scheduled
     * @param system The instance of the whole system
     * @param name The friendly name desired for the thread
     * @param entry_point The address at which the thread should start execution
     * @param priority The thread's priority
     * @param arg User data to pass to the thread
     * @param processor_id The ID(s) of the processors on which the thread is desired to be run
     * @param stack_top The address of the thread's stack top
     * @param owner_process The parent process for the thread, if null, it's a kernel thread
     * @param thread_start_func The function where the host context will start.
     * @param thread_start_parameter The parameter which will passed to host context on init
     * @return A shared pointer to the newly created thread
     */
    [[nodiscard]] static ResultVal<std::shared_ptr<KThread>> Create(
        Core::System& system, ThreadType type_flags, std::string name, VAddr entry_point,
        u32 priority, u64 arg, s32 processor_id, VAddr stack_top, Process* owner_process,
        std::function<void(void*)>&& thread_start_func, void* thread_start_parameter);

    [[nodiscard]] std::string GetName() const override {
        return name;
    }

    void SetName(std::string new_name) {
        name = std::move(new_name);
    }

    [[nodiscard]] std::string GetTypeName() const override {
        return "Thread";
    }

    static constexpr HandleType HANDLE_TYPE = HandleType::Thread;
    [[nodiscard]] HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    /**
     * Gets the thread's current priority
     * @return The current thread's priority
     */
    [[nodiscard]] s32 GetPriority() const {
        return priority;
    }

    /**
     * Sets the thread's current priority.
     * @param priority The new priority.
     */
    void SetPriority(s32 value) {
        priority = value;
    }

    /**
     * Gets the thread's nominal priority.
     * @return The current thread's nominal priority.
     */
    [[nodiscard]] s32 GetBasePriority() const {
        return base_priority;
    }

    /**
     * Gets the thread's thread ID
     * @return The thread's ID
     */
    [[nodiscard]] u64 GetThreadID() const {
        return thread_id;
    }

    void ContinueIfHasKernelWaiters() {
        if (GetNumKernelWaiters() > 0) {
            Continue();
        }
    }

    void Wakeup();

    void SetBasePriority(s32 value);

    [[nodiscard]] ResultCode Run();

    void Exit();

    [[nodiscard]] u32 GetSuspendFlags() const {
        return suspend_allowed_flags & suspend_request_flags;
    }

    [[nodiscard]] bool IsSuspended() const {
        return GetSuspendFlags() != 0;
    }

    [[nodiscard]] bool IsSuspendRequested(SuspendType type) const {
        return (suspend_request_flags &
                (1u << (static_cast<u32>(ThreadState::SuspendShift) + static_cast<u32>(type)))) !=
               0;
    }

    [[nodiscard]] bool IsSuspendRequested() const {
        return suspend_request_flags != 0;
    }

    void RequestSuspend(SuspendType type);

    void Resume(SuspendType type);

    void TrySuspend();

    void Continue();

    void Suspend();

    void Finalize() override;

    bool IsSignaled() const override;

    void SetSyncedObject(KSynchronizationObject* obj, ResultCode wait_res) {
        synced_object = obj;
        wait_result = wait_res;
    }

    [[nodiscard]] ResultCode GetWaitResult(KSynchronizationObject** out) const {
        *out = synced_object;
        return wait_result;
    }

    /*
     * Returns the Thread Local Storage address of the current thread
     * @returns VAddr of the thread's TLS
     */
    [[nodiscard]] VAddr GetTLSAddress() const {
        return tls_address;
    }

    /*
     * Returns the value of the TPIDR_EL0 Read/Write system register for this thread.
     * @returns The value of the TPIDR_EL0 register.
     */
    [[nodiscard]] u64 GetTPIDR_EL0() const {
        return thread_context_64.tpidr;
    }

    /// Sets the value of the TPIDR_EL0 Read/Write system register for this thread.
    void SetTPIDR_EL0(u64 value) {
        thread_context_64.tpidr = value;
        thread_context_32.tpidr = static_cast<u32>(value);
    }

    [[nodiscard]] ThreadContext32& GetContext32() {
        return thread_context_32;
    }

    [[nodiscard]] const ThreadContext32& GetContext32() const {
        return thread_context_32;
    }

    [[nodiscard]] ThreadContext64& GetContext64() {
        return thread_context_64;
    }

    [[nodiscard]] const ThreadContext64& GetContext64() const {
        return thread_context_64;
    }

    [[nodiscard]] std::shared_ptr<Common::Fiber>& GetHostContext();

    [[nodiscard]] ThreadState GetState() const {
        return thread_state & ThreadState::Mask;
    }

    [[nodiscard]] ThreadState GetRawState() const {
        return thread_state;
    }

    void SetState(ThreadState state);

    [[nodiscard]] s64 GetLastScheduledTick() const {
        return last_scheduled_tick;
    }

    void SetLastScheduledTick(s64 tick) {
        last_scheduled_tick = tick;
    }

    void AddCpuTime([[maybe_unused]] s32 core_id_, s64 amount) {
        cpu_time += amount;
        // TODO(bunnei): Debug kernels track per-core tick counts. Should we?
    }

    [[nodiscard]] s64 GetCpuTime() const {
        return cpu_time;
    }

    [[nodiscard]] s32 GetActiveCore() const {
        return core_id;
    }

    void SetActiveCore(s32 core) {
        core_id = core;
    }

    [[nodiscard]] s32 GetCurrentCore() const {
        return current_core_id;
    }

    void SetCurrentCore(s32 core) {
        current_core_id = core;
    }

    [[nodiscard]] Process* GetOwnerProcess() {
        return parent;
    }

    [[nodiscard]] const Process* GetOwnerProcess() const {
        return parent;
    }

    [[nodiscard]] bool IsUserThread() const {
        return parent != nullptr;
    }

    [[nodiscard]] KThread* GetLockOwner() const {
        return lock_owner;
    }

    void SetLockOwner(KThread* owner) {
        lock_owner = owner;
    }

    [[nodiscard]] const KAffinityMask& GetAffinityMask() const {
        return physical_affinity_mask;
    }

    [[nodiscard]] ResultCode GetCoreMask(s32* out_ideal_core, u64* out_affinity_mask);

    [[nodiscard]] ResultCode GetPhysicalCoreMask(s32* out_ideal_core, u64* out_affinity_mask);

    [[nodiscard]] ResultCode SetCoreMask(s32 core_id, u64 v_affinity_mask);

    [[nodiscard]] ResultCode SetActivity(Svc::ThreadActivity activity);

    [[nodiscard]] ResultCode Sleep(s64 timeout);

    [[nodiscard]] s64 GetYieldScheduleCount() const {
        return schedule_count;
    }

    void SetYieldScheduleCount(s64 count) {
        schedule_count = count;
    }

    void WaitCancel();

    [[nodiscard]] bool IsWaitCancelled() const {
        return wait_cancelled;
    }

    [[nodiscard]] void ClearWaitCancelled() {
        wait_cancelled = false;
    }

    [[nodiscard]] bool IsCancellable() const {
        return cancellable;
    }

    void SetCancellable() {
        cancellable = true;
    }

    void ClearCancellable() {
        cancellable = false;
    }

    [[nodiscard]] bool IsTerminationRequested() const {
        return termination_requested || GetRawState() == ThreadState::Terminated;
    }

    struct StackParameters {
        u8 svc_permission[0x10];
        std::atomic<u8> dpc_flags;
        u8 current_svc_id;
        bool is_calling_svc;
        bool is_in_exception_handler;
        bool is_pinned;
        s32 disable_count;
        KThread* cur_thread;
    };

    [[nodiscard]] StackParameters& GetStackParameters() {
        return stack_parameters;
    }

    [[nodiscard]] const StackParameters& GetStackParameters() const {
        return stack_parameters;
    }

    class QueueEntry {
    public:
        constexpr QueueEntry() = default;

        constexpr void Initialize() {
            prev = nullptr;
            next = nullptr;
        }

        constexpr KThread* GetPrev() const {
            return prev;
        }
        constexpr KThread* GetNext() const {
            return next;
        }
        constexpr void SetPrev(KThread* thread) {
            prev = thread;
        }
        constexpr void SetNext(KThread* thread) {
            next = thread;
        }

    private:
        KThread* prev{};
        KThread* next{};
    };

    [[nodiscard]] QueueEntry& GetPriorityQueueEntry(s32 core) {
        return per_core_priority_queue_entry[core];
    }

    [[nodiscard]] const QueueEntry& GetPriorityQueueEntry(s32 core) const {
        return per_core_priority_queue_entry[core];
    }

    void SetSleepingQueue(KThreadQueue* q) {
        sleeping_queue = q;
    }

    [[nodiscard]] s32 GetDisableDispatchCount() const {
        return this->GetStackParameters().disable_count;
    }

    void DisableDispatch() {
        ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() >= 0);
        this->GetStackParameters().disable_count++;
    }

    void EnableDispatch() {
        ASSERT(GetCurrentThread(kernel).GetDisableDispatchCount() > 0);
        this->GetStackParameters().disable_count--;
    }

    void Pin();

    void Unpin();

    void SetInExceptionHandler() {
        this->GetStackParameters().is_in_exception_handler = true;
    }

    void ClearInExceptionHandler() {
        this->GetStackParameters().is_in_exception_handler = false;
    }

    [[nodiscard]] bool IsInExceptionHandler() const {
        return this->GetStackParameters().is_in_exception_handler;
    }

    void SetIsCallingSvc() {
        this->GetStackParameters().is_calling_svc = true;
    }

    void ClearIsCallingSvc() {
        this->GetStackParameters().is_calling_svc = false;
    }

    [[nodiscard]] bool IsCallingSvc() const {
        return this->GetStackParameters().is_calling_svc;
    }

    [[nodiscard]] u8 GetSvcId() const {
        return this->GetStackParameters().current_svc_id;
    }

    void RegisterDpc(DpcFlag flag) {
        this->GetStackParameters().dpc_flags |= static_cast<u8>(flag);
    }

    void ClearDpc(DpcFlag flag) {
        this->GetStackParameters().dpc_flags &= ~static_cast<u8>(flag);
    }

    [[nodiscard]] u8 GetDpc() const {
        return this->GetStackParameters().dpc_flags;
    }

    [[nodiscard]] bool HasDpc() const {
        return this->GetDpc() != 0;
    }

    void SetWaitReasonForDebugging(ThreadWaitReasonForDebugging reason) {
        wait_reason_for_debugging = reason;
    }

    [[nodiscard]] ThreadWaitReasonForDebugging GetWaitReasonForDebugging() const {
        return wait_reason_for_debugging;
    }

    [[nodiscard]] ThreadType GetThreadTypeForDebugging() const {
        return thread_type_for_debugging;
    }

    void SetWaitObjectsForDebugging(const std::span<KSynchronizationObject*>& objects) {
        wait_objects_for_debugging.clear();
        wait_objects_for_debugging.reserve(objects.size());
        for (const auto& object : objects) {
            wait_objects_for_debugging.emplace_back(object);
        }
    }

    [[nodiscard]] const std::vector<KSynchronizationObject*>& GetWaitObjectsForDebugging() const {
        return wait_objects_for_debugging;
    }

    void SetMutexWaitAddressForDebugging(VAddr address) {
        mutex_wait_address_for_debugging = address;
    }

    [[nodiscard]] VAddr GetMutexWaitAddressForDebugging() const {
        return mutex_wait_address_for_debugging;
    }

    [[nodiscard]] s32 GetIdealCoreForDebugging() const {
        return virtual_ideal_core_id;
    }

    void AddWaiter(KThread* thread);

    void RemoveWaiter(KThread* thread);

    [[nodiscard]] ResultCode GetThreadContext3(std::vector<u8>& out);

    [[nodiscard]] KThread* RemoveWaiterByKey(s32* out_num_waiters, VAddr key);

    [[nodiscard]] VAddr GetAddressKey() const {
        return address_key;
    }

    [[nodiscard]] u32 GetAddressKeyValue() const {
        return address_key_value;
    }

    void SetAddressKey(VAddr key) {
        address_key = key;
    }

    void SetAddressKey(VAddr key, u32 val) {
        address_key = key;
        address_key_value = val;
    }

    [[nodiscard]] bool HasWaiters() const {
        return !waiter_list.empty();
    }

    [[nodiscard]] s32 GetNumKernelWaiters() const {
        return num_kernel_waiters;
    }

    [[nodiscard]] u64 GetConditionVariableKey() const {
        return condvar_key;
    }

    [[nodiscard]] u64 GetAddressArbiterKey() const {
        return condvar_key;
    }

private:
    static constexpr size_t PriorityInheritanceCountMax = 10;
    union SyncObjectBuffer {
        std::array<KSynchronizationObject*, Svc::ArgumentHandleCountMax> sync_objects{};
        std::array<Handle,
                   Svc::ArgumentHandleCountMax*(sizeof(KSynchronizationObject*) / sizeof(Handle))>
            handles;
        constexpr SyncObjectBuffer() {}
    };
    static_assert(sizeof(SyncObjectBuffer::sync_objects) == sizeof(SyncObjectBuffer::handles));

    struct ConditionVariableComparator {
        struct LightCompareType {
            u64 cv_key{};
            s32 priority{};

            [[nodiscard]] constexpr u64 GetConditionVariableKey() const {
                return cv_key;
            }

            [[nodiscard]] constexpr s32 GetPriority() const {
                return priority;
            }
        };

        template <typename T>
        requires(
            std::same_as<T, KThread> ||
            std::same_as<T, LightCompareType>) static constexpr int Compare(const T& lhs,
                                                                            const KThread& rhs) {
            const u64 l_key = lhs.GetConditionVariableKey();
            const u64 r_key = rhs.GetConditionVariableKey();

            if (l_key < r_key) {
                // Sort first by key
                return -1;
            } else if (l_key == r_key && lhs.GetPriority() < rhs.GetPriority()) {
                // And then by priority.
                return -1;
            } else {
                return 1;
            }
        }
    };

    void AddWaiterImpl(KThread* thread);

    void RemoveWaiterImpl(KThread* thread);

    void StartTermination();

    [[nodiscard]] ResultCode Initialize(KThreadFunction func, uintptr_t arg, VAddr user_stack_top,
                                        s32 prio, s32 virt_core, Process* owner, ThreadType type);

    [[nodiscard]] static ResultCode InitializeThread(KThread* thread, KThreadFunction func,
                                                     uintptr_t arg, VAddr user_stack_top, s32 prio,
                                                     s32 core, Process* owner, ThreadType type);

    static void RestorePriority(KernelCore& kernel, KThread* thread);

    // For core KThread implementation
    ThreadContext32 thread_context_32{};
    ThreadContext64 thread_context_64{};
    Common::IntrusiveRedBlackTreeNode condvar_arbiter_tree_node{};
    s32 priority{};
    using ConditionVariableThreadTreeTraits =
        Common::IntrusiveRedBlackTreeMemberTraitsDeferredAssert<
            &KThread::condvar_arbiter_tree_node>;
    using ConditionVariableThreadTree =
        ConditionVariableThreadTreeTraits::TreeType<ConditionVariableComparator>;
    ConditionVariableThreadTree* condvar_tree{};
    u64 condvar_key{};
    u64 virtual_affinity_mask{};
    KAffinityMask physical_affinity_mask{};
    u64 thread_id{};
    std::atomic<s64> cpu_time{};
    KSynchronizationObject* synced_object{};
    VAddr address_key{};
    Process* parent{};
    VAddr kernel_stack_top{};
    u32* light_ipc_data{};
    VAddr tls_address{};
    KLightLock activity_pause_lock;
    s64 schedule_count{};
    s64 last_scheduled_tick{};
    std::array<QueueEntry, Core::Hardware::NUM_CPU_CORES> per_core_priority_queue_entry{};
    KThreadQueue* sleeping_queue{};
    WaiterList waiter_list{};
    WaiterList pinned_waiter_list{};
    KThread* lock_owner{};
    u32 address_key_value{};
    u32 suspend_request_flags{};
    u32 suspend_allowed_flags{};
    ResultCode wait_result{RESULT_SUCCESS};
    s32 base_priority{};
    s32 physical_ideal_core_id{};
    s32 virtual_ideal_core_id{};
    s32 num_kernel_waiters{};
    s32 current_core_id{};
    s32 core_id{};
    KAffinityMask original_physical_affinity_mask{};
    s32 original_physical_ideal_core_id{};
    s32 num_core_migration_disables{};
    ThreadState thread_state{};
    std::atomic<bool> termination_requested{};
    bool wait_cancelled{};
    bool cancellable{};
    bool signaled{};
    bool initialized{};
    bool debug_attached{};
    s8 priority_inheritance_count{};
    bool resource_limit_release_hint{};
    StackParameters stack_parameters{};
    Common::SpinLock context_guard{};

    // For emulation
    std::shared_ptr<Common::Fiber> host_context{};

    // For debugging
    std::vector<KSynchronizationObject*> wait_objects_for_debugging;
    VAddr mutex_wait_address_for_debugging{};
    ThreadWaitReasonForDebugging wait_reason_for_debugging{};
    ThreadType thread_type_for_debugging{};
    std::string name;

public:
    using ConditionVariableThreadTreeType = ConditionVariableThreadTree;

    void SetConditionVariable(ConditionVariableThreadTree* tree, VAddr address, u64 cv_key,
                              u32 value) {
        condvar_tree = tree;
        condvar_key = cv_key;
        address_key = address;
        address_key_value = value;
    }

    void ClearConditionVariable() {
        condvar_tree = nullptr;
    }

    [[nodiscard]] bool IsWaitingForConditionVariable() const {
        return condvar_tree != nullptr;
    }

    void SetAddressArbiter(ConditionVariableThreadTree* tree, u64 address) {
        condvar_tree = tree;
        condvar_key = address;
    }

    void ClearAddressArbiter() {
        condvar_tree = nullptr;
    }

    [[nodiscard]] bool IsWaitingForAddressArbiter() const {
        return condvar_tree != nullptr;
    }

    [[nodiscard]] ConditionVariableThreadTree* GetConditionVariableTree() const {
        return condvar_tree;
    }
};

} // namespace Kernel
