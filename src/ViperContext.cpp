#include "ViperContext.h"
#include "log.h"
#include "viper/constants.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

constexpr int32_t kParamGetEnabled = 1;
constexpr int32_t kParamGetConfigure = 2;
constexpr int32_t kParamGetStreaming = 3;
constexpr int32_t kParamGetSamplingRate = 4;
constexpr int32_t kParamGetConvolutionKernelId = 5;
constexpr int32_t kParamGetDriverVersionCode = 6;
constexpr int32_t kParamGetDriverVersionName = 7;
constexpr int32_t kParamGetArchitecture = 8;

// Rounds a size up to the next 32 bit boundary, matching the effect_param_t
// value-offset convention documented in essential.h.
constexpr uint32_t Align4(uint32_t size) noexcept { return (size + 3U) & ~3U; }

// RAII guard that flushes denormals to zero for the lifetime of the object,
// restoring the original FPU/SIMD control state on destruction. Supports
// AArch64, ARMv7 (VFP), and x86/x86_64 (SSE); a no-op elsewhere.
#if defined(__aarch64__)
struct ScopedDenormalFlusher {
    uint64_t orig_fpcr;
    ScopedDenormalFlusher() noexcept {
        asm volatile("mrs %0, fpcr" : "=r"(orig_fpcr));
        asm volatile("msr fpcr, %0" ::"r"(orig_fpcr | (1ULL << 24)));
    }
    ~ScopedDenormalFlusher() noexcept { asm volatile("msr fpcr, %0" ::"r"(orig_fpcr)); }
};
#elif defined(__arm__)
struct ScopedDenormalFlusher {
    uint32_t orig_fpscr;
    ScopedDenormalFlusher() noexcept {
        asm volatile("vmrs %0, fpscr" : "=r"(orig_fpscr));
        asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr | (1U << 24)));
    }
    ~ScopedDenormalFlusher() noexcept { asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr)); }
};
#elif defined(__x86_64__) || defined(_M_X64)
struct ScopedDenormalFlusher {
    unsigned int orig_mxcsr;
    ScopedDenormalFlusher() noexcept {
        orig_mxcsr = _mm_getcsr();
        _mm_setcsr(orig_mxcsr | 0x8040); // FTZ | DAZ
    }
    ~ScopedDenormalFlusher() noexcept { _mm_setcsr(orig_mxcsr); }
};
#else
struct ScopedDenormalFlusher {};
#endif

template <std::integral T>
void PcmToFloat(std::span<float> dst, std::span<const T> src) {
    constexpr float inv_scale = 1.0f / (static_cast<float>(std::numeric_limits<T>::max()) + 1.0f);
    for (auto [d, s] : std::views::zip(dst, src)) {
        d = std::clamp(static_cast<float>(s) * inv_scale, -1.0f, 1.0f);
    }
}

void FloatToFloat(std::span<float> dst, std::span<const float> src, bool accumulate) {
    if (accumulate) {
        for (auto [d, s] : std::views::zip(dst, src)) {
            d = std::clamp(d + s, -1.0f, 1.0f);
        }
    } else {
        std::memcpy(dst.data(), src.data(), src.size() * sizeof(float));
    }
}

template <std::integral T, std::integral U>
void FloatToPcm(std::span<T> dst, std::span<const float> src, bool accumulate) {
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    for (auto [d, s] : std::views::zip(dst, src)) {
        const T pcm = static_cast<T>(std::lrintf(s * static_cast<float>(max_val)));
        if (accumulate) {
            const U temp = static_cast<U>(d) + pcm;
            d = static_cast<T>(
                std::clamp(temp, static_cast<U>(min_val), static_cast<U>(max_val))
            );
        } else {
            d = pcm;
        }
    }
}

audio_buffer_t *GetBuffer(buffer_config_t *config, audio_buffer_t *buffer) {
    if (buffer != nullptr) return buffer;
    if (config->mask & EFFECT_CONFIG_BUFFER) return &config->buffer;
    // EFFECT_CONFIG_PROVIDER not implemented, it's not used by any known effect
    return nullptr;
}

} // namespace

ViperContext::ViperContext() {
    VIPER_LOGI("ViperContext created");
    buffer_.reserve(kDefaultMaxFrames * 2);
}

void ViperContext::CopyBufferConfig(buffer_config_t &dest, const buffer_config_t &src) {
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

std::expected<void, int32_t> ViperContext::HandleSetConfig(const effect_config_t *new_config) {
    VIPER_LOGI("Checking input and output configuration...");

    SetDisableReason(DisableReason::UNKNOWN);

    CopyBufferConfig(config_.input_cfg, new_config->input_cfg);
    CopyBufferConfig(config_.output_cfg, new_config->output_cfg);

    if (config_.input_cfg.buffer.frame_count != config_.output_cfg.buffer.frame_count) {
        const auto msg = std::format(
            "Input and output frame count mismatch: in={}, out={}",
            config_.input_cfg.buffer.frame_count,
            config_.output_cfg.buffer.frame_count
        );
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_FRAME_COUNT, msg);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.sampling_rate != config_.output_cfg.sampling_rate) {
        const auto msg = std::format(
            "Input and output sampling rate mismatch: in={}, out={}",
            config_.input_cfg.sampling_rate,
            config_.output_cfg.sampling_rate
        );
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_SAMPLING_RATE, msg);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.channels != config_.output_cfg.channels) {
        const auto msg = std::format(
            "Input and output channel count mismatch: in={}, out={}",
            config_.input_cfg.channels,
            config_.output_cfg.channels
        );
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_CHANNEL_COUNT, msg);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.channels != AUDIO_CHANNEL_OUT_STEREO) {
        const auto msg = std::format("Invalid channel count: {}", config_.input_cfg.channels);
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_CHANNEL_COUNT, msg);
        return std::unexpected(-EINVAL);
    }

    if (config_.input_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        const auto msg = std::format("Invalid input format: {}", config_.input_cfg.format);
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_FORMAT, msg);
        return std::unexpected(-EINVAL);
    }

    if (config_.output_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        const auto msg = std::format("Invalid output format: {}", config_.output_cfg.format);
        VIPER_LOGE("ViPER4Android disabled, reason [%s]", msg.c_str());
        SetDisableReason(DisableReason::INVALID_FORMAT, msg);
        return std::unexpected(-EINVAL);
    }

    VIPER_LOGI("Input and output configuration verified.");
    SetDisableReason(DisableReason::NONE);

    // Processing buffer
    buffer_.resize(config_.input_cfg.buffer.frame_count * 2);
    buffer_frame_count_ = config_.input_cfg.buffer.frame_count;

    // ViPER
    viper_.SetSamplingRate(config_.input_cfg.sampling_rate);
    viper_.ResetAllEffects();

    return {};
}

std::expected<void, int32_t> ViperContext::HandleSetParam(
    uint32_t cmd_size, const effect_param_t *cmd_param, void *reply_data
) {
    constexpr uint32_t min_cmd_size = sizeof(effect_param_t) + sizeof(int32_t);
    if (cmd_size < min_cmd_size) {
        return std::unexpected(-EINVAL);
    }

    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset = Align4(cmd_param->psize);

    *static_cast<int32_t *>(reply_data) = 0;

    auto read_int32 = [&](uint32_t byte_offset) {
        int32_t value;
        std::memcpy(&value, cmd_param->data + byte_offset, sizeof(int32_t));
        return value;
    };

    const int32_t param = read_int32(0);
    switch (cmd_param->vsize) {
        case sizeof(int32_t): {
            viper_.DispatchRawParam(param, read_int32(offset), 0, 0, 0, nullptr);
            return {};
        }
        case sizeof(int32_t) * 2: {
            viper_.DispatchRawParam(
                param, read_int32(offset), read_int32(offset + sizeof(int32_t)), 0, 0, nullptr
            );
            return {};
        }
        case sizeof(int32_t) * 3: {
            viper_.DispatchRawParam(
                param,
                read_int32(offset),
                read_int32(offset + sizeof(int32_t)),
                read_int32(offset + sizeof(int32_t) * 2),
                0,
                nullptr
            );
            return {};
        }
        case 256:
        case 1024: {
            uint32_t arr_size;
            std::memcpy(&arr_size, cmd_param->data + offset, sizeof(uint32_t));
            auto *arr = reinterpret_cast<signed char *>(
                const_cast<char *>(cmd_param->data) + offset + sizeof(uint32_t)
            );
            viper_.DispatchRawParam(param, 0, 0, 0, arr_size, arr);
            return {};
        }
        case 8192: {
            const int32_t value1 = read_int32(offset);
            uint32_t arr_size;
            std::memcpy(&arr_size, cmd_param->data + offset + sizeof(int32_t), sizeof(uint32_t));
            auto *arr = reinterpret_cast<signed char *>(
                const_cast<char *>(cmd_param->data) + offset + sizeof(int32_t) + sizeof(uint32_t)
            );
            viper_.DispatchRawParam(param, value1, 0, 0, arr_size, arr);
            return {};
        }
        default: {
            return std::unexpected(-EINVAL);
        }
    }
}

std::expected<uint32_t, int32_t> ViperContext::HandleGetParam(
    uint32_t cmd_size,
    const effect_param_t *cmd_param,
    effect_param_t *reply_param,
    uint32_t reply_size_limit
) {
    if (cmd_size < sizeof(effect_param_t) + cmd_param->psize
        || reply_size_limit < sizeof(effect_param_t) + cmd_param->psize) {
        return std::unexpected(-EINVAL);
    }

    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset = Align4(cmd_param->psize);

    std::memcpy(reply_param, cmd_param, sizeof(effect_param_t) + cmd_param->psize);

    uint32_t query;
    std::memcpy(&query, cmd_param->data, sizeof(uint32_t));

    auto write_value = [&](const void *value, uint32_t vsize) -> uint32_t {
        reply_param->status = 0;
        reply_param->vsize = vsize;
        std::memcpy(reply_param->data + offset, value, vsize);
        return sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
    };

    switch (query) {
        case kParamGetEnabled: {
            const int32_t value = enable_.load(std::memory_order_relaxed) ? 1 : 0;
            return write_value(&value, sizeof(int32_t));
        }
        case kParamGetConfigure: {
            const int32_t value =
                disable_reason_.load(std::memory_order_relaxed) == DisableReason::NONE ? 1 : 0;
            return write_value(&value, sizeof(int32_t));
        }
        case kParamGetStreaming: {
            const uint64_t frames = viper_.GetProcessedFrames();
            const int32_t is_processing =
                (frames != last_streaming_frames_ && frames > 0) ? 1 : 0;
            last_streaming_frames_ = frames;
            return write_value(&is_processing, sizeof(int32_t));
        }
        case kParamGetSamplingRate: {
            const uint32_t value = viper_.GetSamplingRate();
            return write_value(&value, sizeof(uint32_t));
        }
        case kParamGetConvolutionKernelId: {
            const uint32_t value = viper_.GetConvolverKernelID();
            return write_value(&value, sizeof(uint32_t));
        }
        case kParamGetDriverVersionCode: {
            const int32_t value = VERSION_CODE;
            return write_value(&value, sizeof(int32_t));
        }
        case kParamGetDriverVersionName: {
            return write_value(VERSION_NAME, static_cast<uint32_t>(strlen(VERSION_NAME)));
        }
        case kParamGetArchitecture: {
            return write_value(VIPER_ARCHITECTURE, sizeof(VIPER_ARCHITECTURE) - 1);
        }
        default: {
            return std::unexpected(-EINVAL);
        }
    }
}

int32_t ViperContext::HandleCommand(
    uint32_t cmd_code,
    uint32_t cmd_size,
    const void *cmd_data,
    uint32_t *reply_size,
    void *reply_data
) {
    const uint32_t rs = reply_size == nullptr ? 0 : *reply_size;

    auto write_status_reply = [&](int32_t status) {
        std::memcpy(reply_data, &status, sizeof(int32_t));
        return 0;
    };

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
            const auto res = HandleSetConfig(static_cast<const effect_config_t *>(cmd_data));
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
            has_processed_ = false;
            fade_in_remaining_ = 0;
            enable_.store(true, std::memory_order_release);
            return write_status_reply(0);
        }
        case EFFECT_CMD_DISABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            enable_.store(false, std::memory_order_release);
            return write_status_reply(0);
        }
        case EFFECT_CMD_SET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            const auto res =
                HandleSetParam(cmd_size, static_cast<const effect_param_t *>(cmd_data), reply_data);
            if (res) {
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
            const auto res = HandleGetParam(
                cmd_size,
                static_cast<const effect_param_t *>(cmd_data),
                static_cast<effect_param_t *>(reply_data),
                rs
            );
            if (res) {
                *reply_size = *res;
                return 0;
            }
            return res.error();
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (rs != sizeof(effect_config_t) || reply_data == nullptr) {
                return -EINVAL;
            }
            *static_cast<effect_config_t *>(reply_data) = config_;
            return 0;
        }
        default: {
            VIPER_LOGE("HandleCommand called with unknown command: %d", cmd_code);
            return -EINVAL;
        }
    }
}

int32_t ViperContext::Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer) {
    if (disable_reason_.load(std::memory_order_relaxed) != DisableReason::NONE) {
        return -EINVAL;
    }

    if (!enable_.load(std::memory_order_acquire)) {
        return -ENODATA;
    }

    in_buffer = GetBuffer(&config_.input_cfg, in_buffer);
    out_buffer = GetBuffer(&config_.output_cfg, out_buffer);
    if (in_buffer == nullptr || out_buffer == nullptr || in_buffer->raw == nullptr
        || out_buffer->raw == nullptr || in_buffer->frame_count != out_buffer->frame_count
        || in_buffer->frame_count == 0) {
        return -EINVAL;
    }

    const ScopedDenormalFlusher denormal_flusher;

    const auto now = std::chrono::steady_clock::now();
    if (has_processed_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_process_time_
        ).count();
        if (elapsed > 100) {
            viper_.ResetAllEffects();
            fade_in_remaining_ = kFadeInFrames;
        }
    } else {
        viper_.ResetAllEffects();
        fade_in_remaining_ = kFadeInFrames;
        has_processed_ = true;
    }
    last_process_time_ = now;

    const size_t frame_count = in_buffer->frame_count;
    const size_t sample_count = frame_count * 2;
    if (frame_count > buffer_frame_count_) {
        buffer_.resize(sample_count);
        buffer_frame_count_ = frame_count;
    }

    switch (config_.input_cfg.format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            PcmToFloat<int16_t>(
                std::span{buffer_}.first(sample_count), std::span{in_buffer->s16, sample_count}
            );
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            PcmToFloat<int32_t>(
                std::span{buffer_}.first(sample_count), std::span{in_buffer->s32, sample_count}
            );
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            FloatToFloat(std::span{buffer_}.first(sample_count), std::span{in_buffer->f32, sample_count}, false);
            break;
        default:
            return -EINVAL;
    }

    // TODO: Remove fade-in.
    if (fade_in_remaining_ > 0) {
        const uint32_t fade_samples =
            fade_in_remaining_ < frame_count ? fade_in_remaining_ : static_cast<uint32_t>(frame_count);
        for (uint32_t i = 0; i < fade_samples; i++) {
            const float gain =
                static_cast<float>(kFadeInFrames - fade_in_remaining_ + i) / static_cast<float>(kFadeInFrames);
            buffer_[i * 2] *= gain;
            buffer_[i * 2 + 1] *= gain;
        }
        fade_in_remaining_ -= fade_samples;
    }

    viper_.Process(buffer_, static_cast<uint32_t>(frame_count));

    const bool accumulate =
        config_.output_cfg.access_mode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    switch (config_.output_cfg.format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            FloatToPcm<int16_t, int32_t>(
                std::span{out_buffer->s16, sample_count}, std::span{buffer_}.first(sample_count), accumulate
            );
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            FloatToPcm<int32_t, int64_t>(
                std::span{out_buffer->s32, sample_count}, std::span{buffer_}.first(sample_count), accumulate
            );
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            FloatToFloat(
                std::span{out_buffer->f32, sample_count}, std::span{buffer_}.first(sample_count), accumulate
            );
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

void ViperContext::SetDisableReason(DisableReason reason, std::string_view message) {
    disable_reason_.store(reason, std::memory_order_release);
    disable_reason_message_ = message;
}
