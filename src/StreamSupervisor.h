#pragma once

#include "viper/ViPER.h"
#include <chrono>
#include <cstdint>
#include <span>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

// ---------------------------------------------------------------------------
// ScopedDenormalFlusher
// RAII guard that flushes denormals to zero for the lifetime of the object,
// restoring the original FPU/SIMD control state on destruction. Supports
// AArch64, ARMv7 (VFP), and x86/x86_64 (SSE); a no-op elsewhere.
// ---------------------------------------------------------------------------
#if defined(__aarch64__)
struct ScopedDenormalFlusher {
    uint64_t orig_fpcr;
    ScopedDenormalFlusher() noexcept {
        asm volatile("mrs %0, fpcr" : "=r"(orig_fpcr));
        asm volatile("msr fpcr, %0" ::"r"(orig_fpcr | (1ULL << 24)));
    }
    ~ScopedDenormalFlusher() noexcept { asm volatile("msr fpcr, %0" ::"r"(orig_fpcr)); }
    ScopedDenormalFlusher(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher& operator=(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher(ScopedDenormalFlusher&&) = delete;
    ScopedDenormalFlusher& operator=(ScopedDenormalFlusher&&) = delete;
};
#elif defined(__arm__)
struct ScopedDenormalFlusher {
    uint32_t orig_fpscr;
    ScopedDenormalFlusher() noexcept {
        asm volatile("vmrs %0, fpscr" : "=r"(orig_fpscr));
        asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr | (1U << 24)));
    }
    ~ScopedDenormalFlusher() noexcept { asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr)); }
    ScopedDenormalFlusher(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher& operator=(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher(ScopedDenormalFlusher&&) = delete;
    ScopedDenormalFlusher& operator=(ScopedDenormalFlusher&&) = delete;
};
#elif defined(__x86_64__) || defined(_M_X64)
struct ScopedDenormalFlusher {
    unsigned int orig_mxcsr;
    ScopedDenormalFlusher() noexcept {
        orig_mxcsr = _mm_getcsr();
        _mm_setcsr(orig_mxcsr | 0x8040); // FTZ | DAZ
    }
    ~ScopedDenormalFlusher() noexcept { _mm_setcsr(orig_mxcsr); }
    ScopedDenormalFlusher(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher& operator=(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher(ScopedDenormalFlusher&&) = delete;
    ScopedDenormalFlusher& operator=(ScopedDenormalFlusher&&) = delete;
};
#else
struct ScopedDenormalFlusher {
    ScopedDenormalFlusher() = default;
    ~ScopedDenormalFlusher() = default;
    ScopedDenormalFlusher(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher& operator=(const ScopedDenormalFlusher&) = delete;
    ScopedDenormalFlusher(ScopedDenormalFlusher&&) = delete;
    ScopedDenormalFlusher& operator=(ScopedDenormalFlusher&&) = delete;
};
#endif

// ---------------------------------------------------------------------------
// StreamSupervisor
// Manages stream-level health: gap detection, DSP reset triggering, and the
// fade-in ramp applied to the first kFadeInFrames frames after each reset.
// ---------------------------------------------------------------------------
class StreamSupervisor {
public:
    // Number of frames over which the fade-in ramp is applied after a reset.
    static constexpr uint32_t kFadeInFrames = 128;

    // Call at EFFECT_CMD_ENABLE time to clear all timing state.
    void OnStreamEnable() noexcept {
        has_processed_ = false;
        fade_in_remaining_ = 0;
    }

    // Call at the start of each Process() frame.
    // If >100 ms has elapsed since the previous call (stream discontinuity),
    // or this is the very first call after enable, triggers dsp.ResetAllEffects()
    // and arms the fade-in counter.
    void ValidateTiming(ViPER& dsp) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (has_processed_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_process_time_
            ).count();
            if (elapsed > 100) {
                dsp.ResetAllEffects();
                fade_in_remaining_ = kFadeInFrames;
            }
        } else {
            dsp.ResetAllEffects();
            fade_in_remaining_ = kFadeInFrames;
            has_processed_ = true;
        }
        last_process_time_ = now;
    }

    // Apply a linear fade-in ramp to the first kFadeInFrames frames.
    // buf must be interleaved stereo (2 samples per frame).
    // Modifies buf in-place; no-op once fade_in_remaining_ reaches 0.
    // TODO: Remove fade-in.
    void ApplyFadeIn(std::span<float> buf, size_t frame_count) noexcept {
        if (fade_in_remaining_ == 0) return;
        const uint32_t fade_samples =
            fade_in_remaining_ < static_cast<uint32_t>(frame_count)
                ? fade_in_remaining_
                : static_cast<uint32_t>(frame_count);
        for (uint32_t i = 0; i < fade_samples; i++) {
            const float gain =
                static_cast<float>(kFadeInFrames - fade_in_remaining_ + i)
                    / static_cast<float>(kFadeInFrames);
            buf[i * 2]     *= gain;
            buf[i * 2 + 1] *= gain;
        }
        fade_in_remaining_ -= fade_samples;
    }

private:
    std::chrono::steady_clock::time_point last_process_time_;
    bool     has_processed_{false};
    uint32_t fade_in_remaining_{0};
};
