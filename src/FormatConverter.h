#pragma once

#include "essential.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>

namespace FormatConverter {

namespace detail {

template <std::integral T>
void PcmToFloat(std::span<float> dst, std::span<const T> src) noexcept {
    constexpr float inv_scale = 1.0f / (static_cast<float>(std::numeric_limits<T>::max()) + 1.0f);
    for (auto [d, s] : std::views::zip(dst, src)) {
        d = std::clamp(static_cast<float>(s) * inv_scale, -1.0f, 1.0f);
    }
}

inline void FloatToFloat(std::span<float> dst, std::span<const float> src, bool accumulate) noexcept {
    if (accumulate) {
        for (auto [d, s] : std::views::zip(dst, src)) {
            d = std::clamp(d + s, -1.0f, 1.0f);
        }
    } else {
        std::memcpy(dst.data(), src.data(), src.size() * sizeof(float));
    }
}

template <std::integral T, std::integral U>
void FloatToPcm(std::span<T> dst, std::span<const float> src, bool accumulate) noexcept {
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    for (auto [d, s] : std::views::zip(dst, src)) {
        // Pre-clamp to [-1, 1] before scaling: lrintf(s * max_val) is UB when
        // s * max_val overflows the integer range (e.g. s=2.5f from DSP gain).
        const float clamped = std::clamp(s, -1.0f, 1.0f);
        const T pcm = static_cast<T>(std::lrintf(clamped * static_cast<float>(max_val)));
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

} // namespace detail

// Dispatch: reads in_buffer raw data, writes converted samples into float dst span.
// format is the audio_format_t value from buffer_config_t::format (uint8_t).
inline void ToFloat(std::span<float> dst, const audio_buffer_t& src, uint8_t format) noexcept {
    const size_t sample_count = dst.size();
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            detail::PcmToFloat<int16_t>(dst, std::span{src.s16, sample_count});
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            detail::PcmToFloat<int32_t>(dst, std::span{src.s32, sample_count});
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            detail::FloatToFloat(dst, std::span{src.f32, sample_count}, false);
            break;
        default:
            break;
    }
}

// Dispatch: reads float src span, writes converted samples to out_buffer.
// format is the audio_format_t value from buffer_config_t::format (uint8_t).
inline void FromFloat(audio_buffer_t& dst, std::span<const float> src, uint8_t format, bool accumulate) noexcept {
    const size_t sample_count = src.size();
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            detail::FloatToPcm<int16_t, int32_t>(std::span{dst.s16, sample_count}, src, accumulate);
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            detail::FloatToPcm<int32_t, int64_t>(std::span{dst.s32, sample_count}, src, accumulate);
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            detail::FloatToFloat(std::span{dst.f32, sample_count}, src, accumulate);
            break;
        default:
            break;
    }
}

} // namespace FormatConverter
