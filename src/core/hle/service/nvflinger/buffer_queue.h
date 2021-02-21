// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <vector>

#include "common/common_funcs.h"
#include "common/math_util.h"
#include "common/swap.h"
#include "core/hle/kernel/object.h"
#include "core/hle/service/nvdrv/nvdata.h"

namespace Kernel {
class KernelCore;
class KEvent;
class KReadableEvent;
class KWritableEvent;
} // namespace Kernel

namespace Service::NVFlinger {

constexpr u32 buffer_slots = 0x40;
struct IGBPBuffer {
    u32_le magic;
    u32_le width;
    u32_le height;
    u32_le stride;
    u32_le format;
    u32_le usage;
    INSERT_PADDING_WORDS(1);
    u32_le index;
    INSERT_PADDING_WORDS(3);
    u32_le gpu_buffer_id;
    INSERT_PADDING_WORDS(17);
    u32_le nvmap_handle;
    u32_le offset;
    INSERT_PADDING_WORDS(60);
};

static_assert(sizeof(IGBPBuffer) == 0x16C, "IGBPBuffer has wrong size");

class BufferQueue final {
public:
    enum class QueryType {
        NativeWindowWidth = 0,
        NativeWindowHeight = 1,
        NativeWindowFormat = 2,
    };

    explicit BufferQueue(Kernel::KernelCore& kernel, u32 id, u64 layer_id);
    ~BufferQueue();

    enum class BufferTransformFlags : u32 {
        /// No transform flags are set
        Unset = 0x00,
        /// Flip source image horizontally (around the vertical axis)
        FlipH = 0x01,
        /// Flip source image vertically (around the horizontal axis)
        FlipV = 0x02,
        /// Rotate source image 90 degrees clockwise
        Rotate90 = 0x04,
        /// Rotate source image 180 degrees
        Rotate180 = 0x03,
        /// Rotate source image 270 degrees clockwise
        Rotate270 = 0x07,
    };

    enum class PixelFormat : u32 {
        RGBA8888 = 1,
        RGBX8888 = 2,
        RGB888 = 3,
        RGB565 = 4,
        BGRA8888 = 5,
        RGBA5551 = 6,
        RRGBA4444 = 7,
    };

    struct Buffer {
        enum class Status { Free = 0, Queued = 1, Dequeued = 2, Acquired = 3 };

        u32 slot;
        Status status = Status::Free;
        IGBPBuffer igbp_buffer;
        BufferTransformFlags transform;
        Common::Rectangle<int> crop_rect;
        u32 swap_interval;
        Service::Nvidia::MultiFence multi_fence;
    };

    void SetPreallocatedBuffer(u32 slot, const IGBPBuffer& igbp_buffer);
    std::optional<std::pair<u32, Service::Nvidia::MultiFence*>> DequeueBuffer(u32 width,
                                                                              u32 height);
    const IGBPBuffer& RequestBuffer(u32 slot) const;
    void QueueBuffer(u32 slot, BufferTransformFlags transform,
                     const Common::Rectangle<int>& crop_rect, u32 swap_interval,
                     Service::Nvidia::MultiFence& multi_fence);
    void CancelBuffer(u32 slot, const Service::Nvidia::MultiFence& multi_fence);
    std::optional<std::reference_wrapper<const Buffer>> AcquireBuffer();
    void ReleaseBuffer(u32 slot);
    void Connect();
    void Disconnect();
    u32 Query(QueryType type);

    u32 GetId() const {
        return id;
    }

    bool IsConnected() const {
        return is_connect;
    }

    std::shared_ptr<Kernel::KWritableEvent> GetWritableBufferWaitEvent() const;

    std::shared_ptr<Kernel::KReadableEvent> GetBufferWaitEvent() const;

private:
    BufferQueue(const BufferQueue&) = delete;

    u32 id{};
    u64 layer_id{};
    std::atomic_bool is_connect{};

    std::list<u32> free_buffers;
    std::array<Buffer, buffer_slots> buffers;
    std::list<u32> queue_sequence;
    std::shared_ptr<Kernel::KEvent> buffer_wait_event;

    std::mutex free_buffers_mutex;
    std::condition_variable free_buffers_condition;

    std::mutex queue_sequence_mutex;
};

} // namespace Service::NVFlinger
