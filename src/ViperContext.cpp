#include "ViperContext.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ranges>
#include <span>
#include <utility>

namespace {

audio_buffer_t* GetBuffer(buffer_config_t* config, audio_buffer_t* buffer) noexcept {
    if (buffer != nullptr) return buffer;
    if (config->mask & EFFECT_CONFIG_BUFFER) return &config->buffer;
    // EFFECT_CONFIG_PROVIDER not implemented, it's not used by any known effect
    return nullptr;
}

} // namespace

ViperContext::ViperContext() {
    VIPER_LOGI("ViperContext created");
    // Pre-allocate to maximum capacity so Process() never calls resize() on the RT thread.
    buffer_.resize(kDefaultMaxFrames * 2, 0.0f);
    buffer_frame_count_ = kDefaultMaxFrames;
}

void ViperContext::CopyBufferConfig(buffer_config_t& dest, const buffer_config_t& src) noexcept {
    if (src.mask & EFFECT_CONFIG_BUFFER) {
        dest.buffer = src.buffer;
    }
    if (src.mask & EFFECT_CONFIG_SMP_RATE) {
        dest.sampling_rate = src.sampling_rate;
    }
    if (src.mask & EFFECT_CONFIG_CHANNELS) {
        dest.channels = src.channels;
    }
    if (src.mask & EFFECT_CONFIG_FORMAT) {
        dest.format = src.format;
    }
    if (src.mask & EFFECT_CONFIG_ACC_MODE) {
        dest.access_mode = src.access_mode;
    }
    if (src.mask & EFFECT_CONFIG_PROVIDER) {
        dest.buffer_provider = src.buffer_provider;
    }
    dest.mask |= src.mask;
}

std::expected<void, int32_t> ViperContext::HandleSetConfig(const effect_config_t* new_config) {
    VIPER_LOGI("Checking input and output configuration...");

    SetDisableReason(DisableReason::UNKNOWN);

    CopyBufferConfig(config_.input_cfg, new_config->input_cfg);
    CopyBufferConfig(config_.output_cfg, new_config->output_cfg);

    VIPER_LOGD(
        "input: frames=%zu rate=%u channels=0x%x format=%u access=%u",
        config_.input_cfg.buffer.frame_count,
        config_.input_cfg.sampling_rate,
        config_.input_cfg.channels,
        config_.input_cfg.format,
        std::to_underlying(config_.input_cfg.access_mode)
    );
    VIPER_LOGD(
        "output: frames=%zu rate=%u channels=0x%x format=%u access=%u",
        config_.output_cfg.buffer.frame_count,
        config_.output_cfg.sampling_rate,
        config_.output_cfg.channels,
        config_.output_cfg.format,
        std::to_underlying(config_.output_cfg.access_mode)
    );

    if (config_.input_cfg.buffer.frame_count != config_.output_cfg.buffer.frame_count) {
        VIPER_LOGE("ViPER4Android disabled: input/output frame count mismatch");
        SetDisableReason(DisableReason::INVALID_FRAME_COUNT);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.sampling_rate != config_.output_cfg.sampling_rate) {
        VIPER_LOGE("ViPER4Android disabled: input/output sampling rate mismatch");
        SetDisableReason(DisableReason::INVALID_SAMPLING_RATE);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.channels != config_.output_cfg.channels) {
        VIPER_LOGE("ViPER4Android disabled: input/output channel count mismatch");
        SetDisableReason(DisableReason::INVALID_CHANNEL_COUNT);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.channels != AUDIO_CHANNEL_OUT_STEREO) {
        VIPER_LOGE("ViPER4Android disabled: invalid channel count");
        SetDisableReason(DisableReason::INVALID_CHANNEL_COUNT);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        VIPER_LOGE("ViPER4Android disabled: invalid input format");
        SetDisableReason(DisableReason::INVALID_FORMAT);
        return std::unexpected(-EINVAL);
    }

    if (config_.output_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        VIPER_LOGE("ViPER4Android disabled: invalid output format");
        SetDisableReason(DisableReason::INVALID_FORMAT);
        return std::unexpected(-EINVAL);
    }

    VIPER_LOGI("Input and output configuration verified.");
    SetDisableReason(DisableReason::NONE);

    // Processing buffer — never shrink below kDefaultMaxFrames.
    // The constructor pre-allocates kDefaultMaxFrames * 2. If SET_CONFIG arrives
    // with a smaller frame_count (e.g. 192), shrinking would cause Process() to
    // reject larger bursts that arrive later (frame_count > buffer_.size() / 2).
    const size_t new_frame_count = config_.input_cfg.buffer.frame_count;
    if (new_frame_count * 2 > buffer_.size()) {
        buffer_.resize(new_frame_count * 2, 0.0f);
    }
    buffer_frame_count_ = new_frame_count;

    // ViPER
    viper_.SetSamplingRate(config_.input_cfg.sampling_rate);
    viper_.ResetAllEffects();

    return {};
}

int32_t ViperContext::HandleCommand(
    uint32_t cmd_code,
    uint32_t cmd_size,
    const std::byte* cmd_data,
    uint32_t* reply_size,
    std::byte* reply_data
) noexcept {
    const uint32_t rs = reply_size == nullptr ? 0 : *reply_size;

    auto write_status_reply = [&](int32_t status) noexcept {
        std::memcpy(reply_data, &status, sizeof(int32_t));
        return 0;
    };
    // Alias typed pointers to the raw byte buffers for structured access.
    const auto* typed_cmd  = reinterpret_cast<const void*>(cmd_data);
    auto*       typed_rply = reinterpret_cast<void*>(reply_data);

    switch (cmd_code) {
        case EFFECT_CMD_INIT: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            return write_status_reply(0);
        }
        case EFFECT_CMD_SET_CONFIG: {
            if (cmd_size < sizeof(effect_config_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            const auto res = HandleSetConfig(static_cast<const effect_config_t*>(typed_cmd));
            return write_status_reply(res.error_or(0));
        }
        case EFFECT_CMD_RESET: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            viper_.ResetAllEffects();
            return write_status_reply(0);
        }
        case EFFECT_CMD_ENABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            viper_.ResetAllEffects();
            std::ranges::fill(buffer_, 0.0f);
            supervisor_.OnStreamEnable();
            enable_.store(true);
            return write_status_reply(0);
        }
        case EFFECT_CMD_DISABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            enable_.store(false);
            return write_status_reply(0);
        }
        case EFFECT_CMD_SET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            const auto res = ParameterRouter::HandleSet(
                cmd_size,
                static_cast<const effect_param_t*>(typed_cmd),
                static_cast<effect_param_t*>(typed_rply),
                viper_
            );
            if (res.has_value()) {
                *reply_size = sizeof(int32_t);
                return 0;
            }
            return write_status_reply(res.error_or(0));
        }
        case EFFECT_CMD_GET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs < sizeof(effect_param_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            const auto res = ParameterRouter::HandleGet({
                .cmd_size          = cmd_size,
                .cmd_param         = static_cast<const effect_param_t*>(typed_cmd),
                .reply_param       = static_cast<effect_param_t*>(typed_rply),
                .reply_size_limit  = rs,
                .dsp               = viper_,
                .is_enabled        = enable_.load(),
                .disable_reason    = disable_reason_.load(),
                .last_streaming_frames = last_streaming_frames_,
            });
            if (res.has_value()) {
                *reply_size = *res;
                return 0;
            }
            return res.error();
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (rs != sizeof(effect_config_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            *static_cast<effect_config_t*>(typed_rply) = config_;
            return 0;
        }
        default: {
            VIPER_LOGE("HandleCommand called with unknown command: %d", cmd_code);
            return -EINVAL;
        }
    }
}

int32_t ViperContext::Process(audio_buffer_t* in_buffer, audio_buffer_t* out_buffer) noexcept {
    if (disable_reason_.load() != DisableReason::NONE) {
        return -EINVAL;
    }
    if (!enable_.load()) {
        return -ENODATA;
    }

    in_buffer  = GetBuffer(&config_.input_cfg,  in_buffer);
    out_buffer = GetBuffer(&config_.output_cfg, out_buffer);
    if (in_buffer == nullptr || out_buffer == nullptr
        || in_buffer->raw == nullptr || out_buffer->raw == nullptr
        || in_buffer->frame_count != out_buffer->frame_count
        || in_buffer->frame_count == 0) {
        return -EINVAL;
    }

    const ScopedDenormalFlusher denormal_guard;
    supervisor_.ValidateTiming(viper_);

    const size_t sample_count = in_buffer->frame_count * 2;
    // Never allocate on the RT audio thread. buffer_ is pre-sized in the constructor
    // and may grow in HandleSetConfig (non-RT path). Reject oversized frames here.
    if (sample_count > buffer_.size()) {
        return -EINVAL;
    }

    FormatConverter::ToFloat(
        std::span{buffer_}.first(sample_count),
        *in_buffer,
        config_.input_cfg.format
    );
    supervisor_.ApplyFadeIn(std::span{buffer_}.first(sample_count), in_buffer->frame_count);
    viper_.Process(buffer_, static_cast<uint32_t>(in_buffer->frame_count));

    const bool acc = config_.output_cfg.access_mode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    FormatConverter::FromFloat(
        *out_buffer,
        std::span{buffer_}.first(sample_count),
        config_.output_cfg.format,
        acc
    );
    return 0;
}

void ViperContext::SetDisableReason(DisableReason reason) noexcept {
    disable_reason_.store(reason);
}
