// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/service/audio/audin_u.h"

namespace Service::Audio {

class IAudioIn final : public ServiceFramework<IAudioIn> {
public:
    explicit IAudioIn(Core::System& system_) : ServiceFramework{system_, "IAudioIn"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetAudioInState"},
            {1, nullptr, "StartAudioIn"},
            {2, nullptr, "StopAudioIn"},
            {3, nullptr, "AppendAudioInBuffer"},
            {4, nullptr, "RegisterBufferEvent"},
            {5, nullptr, "GetReleasedAudioInBuffer"},
            {6, nullptr, "ContainsAudioInBuffer"},
            {7, nullptr, "AppendAudioInBufferWithUserEvent"},
            {8, nullptr, "AppendAudioInBufferAuto"},
            {9, nullptr, "GetReleasedAudioInBufferAuto"},
            {10, nullptr, "AppendAudioInBufferWithUserEventAuto"},
            {11, nullptr, "GetAudioInBufferCount"},
            {12, nullptr, "SetAudioInDeviceGain"},
            {13, nullptr, "GetAudioInDeviceGain"},
            {14, nullptr, "FlushAudioInBuffers"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

AudInU::AudInU(Core::System& system_) : ServiceFramework{system_, "audin:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &AudInU::ListAudioIns, "ListAudioIns"},
        {1, &AudInU::OpenAudioIn, "OpenAudioIn"},
        {2, &AudInU::ListAudioIns, "ListAudioInsAuto"},
        {3, &AudInU::OpenAudioIn, "OpenAudioInAuto"},
        {4, &AudInU::ListAudioInsAutoFiltered, "ListAudioInsAutoFiltered"},
        {5, &AudInU::OpenAudioInProtocolSpecified, "OpenAudioInProtocolSpecified"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

AudInU::~AudInU() = default;

void AudInU::ListAudioIns(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_Audio, "called");
    const std::size_t count = ctx.GetWriteBufferSize() / sizeof(AudioInDeviceName);

    const std::size_t device_count = std::min(count, audio_device_names.size());
    std::vector<AudioInDeviceName> device_names;
    device_names.reserve(device_count);

    for (std::size_t i = 0; i < device_count; i++) {
        const auto& device_name = audio_device_names[i];
        auto& entry = device_names.emplace_back();
        device_name.copy(entry.data(), device_name.size());
    }

    ctx.WriteBuffer(device_names);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(static_cast<u32>(device_names.size()));
}

void AudInU::ListAudioInsAutoFiltered(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_Audio, "called");
    constexpr u32 device_count = 0;

    // Since we don't actually use any other audio input devices, we return 0 devices. Filtered
    // device listing just omits the default input device

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(static_cast<u32>(device_count));
}

void AudInU::OpenInOutImpl(Kernel::HLERequestContext& ctx) {
    AudInOutParams params{};
    params.channel_count = 2;
    params.sample_format = SampleFormat::PCM16;
    params.sample_rate = 48000;
    params.state = State::Started;

    IPC::ResponseBuilder rb{ctx, 6, 0, 1};
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw<AudInOutParams>(params);
    rb.PushIpcInterface<IAudioIn>(system);
}

void AudInU::OpenAudioIn(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_Audio, "(STUBBED) called");
    OpenInOutImpl(ctx);
}

void AudInU::OpenAudioInProtocolSpecified(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_Audio, "(STUBBED) called");
    OpenInOutImpl(ctx);
}

} // namespace Service::Audio
