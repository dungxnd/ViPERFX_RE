#pragma once

#include "essential.h"
#include "viper/ViPER.h"
#include <atomic>
#include <chrono>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ViperContext {
public:
    enum class DisableReason : int32_t {
        UNKNOWN = -1,
        NONE = 0,
        INVALID_FRAME_COUNT,
        INVALID_SAMPLING_RATE,
        INVALID_CHANNEL_COUNT,
        INVALID_FORMAT,
    };

    static constexpr uint32_t kFadeInFrames = 128;
    static constexpr size_t kDefaultMaxFrames = 4096;

    ViperContext();

    ViperContext(const ViperContext &) = delete;
    ViperContext &operator=(const ViperContext &) = delete;
    ViperContext(ViperContext &&) = delete;
    ViperContext &operator=(ViperContext &&) = delete;

    [[nodiscard]] int32_t HandleCommand(
        uint32_t cmd_code,
        uint32_t cmd_size,
        const void *cmd_data,
        uint32_t *reply_size,
        void *reply_data
    ) noexcept;
    [[nodiscard]] int32_t Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer) noexcept;

private:
    effect_config_t config_{};
    std::atomic<DisableReason> disable_reason_{DisableReason::NONE};
    std::string disable_reason_message_;

    // Processing buffer
    std::vector<float> buffer_;
    size_t buffer_frame_count_{0};

    // Viper
    std::atomic<bool> enable_{false};
    ViPER viper_;
    uint64_t last_streaming_frames_ = 0;

    // Stream discontinuity detection
    std::chrono::steady_clock::time_point last_process_time_;
    bool has_processed_{false};
    uint32_t fade_in_remaining_{0};

    static void CopyBufferConfig(buffer_config_t &dest, const buffer_config_t &src) noexcept;

    [[nodiscard]] std::expected<void, int32_t> HandleSetConfig(
        const effect_config_t *new_config
    );

    [[nodiscard]] std::expected<void, int32_t> HandleSetParam(
        uint32_t cmd_size, const effect_param_t *cmd_param, void *reply_data
    ) noexcept;

    [[nodiscard]] std::expected<uint32_t, int32_t> HandleGetParam(
        uint32_t cmd_size,
        const effect_param_t *cmd_param,
        effect_param_t *reply_param,
        uint32_t reply_size_limit
    ) noexcept;

    void SetDisableReason(DisableReason reason, std::string_view message = "");
};
