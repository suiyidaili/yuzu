// Copyright 2019 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/time/steady_clock_core.h"
#include "core/hle/service/time/system_clock_context_update_callback.h"
#include "core/hle/service/time/system_clock_core.h"

namespace Service::Time::Clock {

SystemClockCore::SystemClockCore(SteadyClockCore& steady_clock_core)
    : steady_clock_core{steady_clock_core} {
    context.steady_time_point.clock_source_id = steady_clock_core.GetClockSourceId();
}

SystemClockCore ::~SystemClockCore() = default;

ResultCode SystemClockCore::GetCurrentTime(Core::System& system, s64& posix_time) const {
    posix_time = 0;

    const SteadyClockTimePoint current_time_point{steady_clock_core.GetCurrentTimePoint(system)};

    SystemClockContext clock_context{};
    if (const ResultCode result{GetClockContext(system, clock_context)}; result != RESULT_SUCCESS) {
        return result;
    }

    if (current_time_point.clock_source_id != clock_context.steady_time_point.clock_source_id) {
        return ERROR_TIME_MISMATCH;
    }

    posix_time = clock_context.offset + current_time_point.time_point;

    return RESULT_SUCCESS;
}

ResultCode SystemClockCore::SetCurrentTime(Core::System& system, s64 posix_time) {
    const SteadyClockTimePoint current_time_point{steady_clock_core.GetCurrentTimePoint(system)};
    const SystemClockContext clock_context{posix_time - current_time_point.time_point,
                                           current_time_point};

    if (const ResultCode result{SetClockContext(clock_context)}; result != RESULT_SUCCESS) {
        return result;
    }
    return Flush(clock_context);
}

ResultCode SystemClockCore::Flush(const SystemClockContext& context) {
    if (!system_clock_context_update_callback) {
        return RESULT_SUCCESS;
    }
    return system_clock_context_update_callback->Update(context);
}

ResultCode SystemClockCore::SetSystemClockContext(const SystemClockContext& context) {
    if (const ResultCode result{SetClockContext(context)}; result != RESULT_SUCCESS) {
        return result;
    }
    return Flush(context);
}

bool SystemClockCore::IsClockSetup(Core::System& system) const {
    SystemClockContext value{};
    if (GetClockContext(system, value) == RESULT_SUCCESS) {
        const SteadyClockTimePoint steady_clock_time_point{
            steady_clock_core.GetCurrentTimePoint(system)};
        return steady_clock_time_point.clock_source_id == value.steady_time_point.clock_source_id;
    }
    return {};
}

} // namespace Service::Time::Clock
