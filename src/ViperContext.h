#pragma once

#include "essential.h"
#include "viper/ViPER.h"
#include "FormatConverter.h"
#include "ParameterRouter.h"
#include "StreamSupervisor.h"
#include <atomic>
#include <cstddef>
#include <expected>
#include <vector>

class ViperContext {
public:
    using DisableReason = ParameterRouter::DisableReason;

    static constexpr uint32_t kFadeInFrames  = StreamSupervisor::kFadeInFrames;
    static constexpr size_t   kDefaultMaxFrames = 4096;

    ViperContext();

    ViperContext(const ViperContext&) = delete;
    ViperContext& operator=(const ViperContext&) = delete;
    ViperContext(ViperContext&&) = delete;
    ViperContext& operator=(ViperContext&&) = delete;

    [[nodiscard]] int32_t HandleCommand(
        uint32_t cmd_code,
        uint32_t cmd_size,
        const std::byte* cmd_data,
        uint32_t* reply_size,
        std::byte* reply_data
    ) noexcept;

    [[nodiscard]] int32_t Process(audio_buffer_t* in_buffer, audio_buffer_t* out_buffer) noexcept;

    // Accessor for the AIDL worker loop to apply SHM param snapshots and
    // load DDC/convolver data directly without going through the legacy
    // EFFECT_CMD_SET_PARAM serialisation layer.
    ViPER& viper() noexcept { return viper_; }

private:
    effect_config_t config_{};
    std::atomic<DisableReason> disable_reason_{DisableReason::NONE};

    // Processing buffer (pre-allocated; never resized on the RT thread)
    std::vector<float> buffer_;
    size_t buffer_frame_count_{0};

    std::atomic<bool> enable_{false};
    ViPER viper_;
    uint64_t last_streaming_frames_{0};

    StreamSupervisor supervisor_;

    static void CopyBufferConfig(buffer_config_t& dest, const buffer_config_t& src) noexcept;
    [[nodiscard]] std::expected<void, int32_t> HandleSetConfig(const effect_config_t* new_config);
    void SetDisableReason(DisableReason reason) noexcept;
};
