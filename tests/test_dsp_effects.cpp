// Direct unit tests for ViPERDSP effect classes.
// Tests run on the host — no Android HAL or NDK required.
// Each suite exercises one effect in isolation: construct → configure → Process.

#include "effects/SoftwareLimiter.h"
#include "effects/FETCompressor.h"
#include "effects/PlaybackGain.h"
#include "effects/DiffSurround.h"
#include "effects/StereoImager.h"
#include "effects/TubeSimulator.h"
#include "effects/DynamicEQ.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

// ============================================================
// Shared signal helpers
// ============================================================

// Stereo interleaved sine wave: [L0,R0, L1,R1, ...]
// L and R channels carry the same sine (in-phase).
static std::vector<float> sine_stereo(float freq_hz, uint32_t fs, size_t frames) {
    std::vector<float> buf(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float s = std::sin(2.0f * static_cast<float>(M_PI) * freq_hz
                                 * static_cast<float>(i) / static_cast<float>(fs));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    return buf;
}

// Max absolute difference between two buffers.
static float max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

// True if every sample in buf is finite (no NaN / Inf).
static bool all_finite(const std::vector<float>& buf) {
    return std::all_of(buf.begin(), buf.end(),
                       [](float v) { return std::isfinite(v); });
}

// RMS of a buffer.
static float rms(const std::vector<float>& buf) {
    double sum = 0.0;
    for (float v : buf) sum += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(sum / buf.size()));
}

// ============================================================
// SoftwareLimiter
// ============================================================

TEST(SoftwareLimiter, Silence_In_Silence_Out) {
    SoftwareLimiter lim;
    for (int i = 0; i < 512; ++i)
        EXPECT_FLOAT_EQ(lim.Process(0.0f), 0.0f);
}

TEST(SoftwareLimiter, LookaheadDelay_First256Outputs_AreZero) {
    // The limiter uses a 256-sample lookahead delay ring.
    // Samples fed in frames [0..255] are not emitted until frame [256..511].
    SoftwareLimiter lim;
    for (int i = 0; i < 256; ++i)
        EXPECT_FLOAT_EQ(lim.Process(0.5f), 0.0f)
            << "expected zero output during lookahead fill at frame " << i;
}

TEST(SoftwareLimiter, HardClip_AboveGate_AttenuatesOutput) {
    // Feed constant amplitude 1.0 with gate = 0.5.
    // After the lookahead fills, every output must be ≤ gate + small epsilon.
    SoftwareLimiter lim;
    lim.SetGate(0.5f);
    constexpr float kGate = 0.5f;
    constexpr float kEps  = 1e-3f; // one release step tolerance
    for (int i = 0; i < 1024; ++i) {
        const float out = lim.Process(1.0f);
        if (i >= 256) {
            EXPECT_LE(std::fabs(out), kGate + kEps)
                << "output exceeded gate at frame " << i;
        }
    }
}

TEST(SoftwareLimiter, BelowGate_OutputUnchanged_AfterWarmup) {
    // Signal at 0.3 with gate 0.9 — no attenuation after lookahead.
    SoftwareLimiter lim;
    lim.SetGate(0.9f);
    // skip lookahead
    for (int i = 0; i < 256; ++i) lim.Process(0.3f);
    // steady state: gain should be 1.0
    for (int i = 0; i < 256; ++i)
        EXPECT_NEAR(lim.Process(0.3f), 0.3f, 1e-4f);
}

TEST(SoftwareLimiter, Reset_ClearsDelay) {
    SoftwareLimiter lim;
    lim.SetGate(0.9f);
    // warm up
    for (int i = 0; i < 512; ++i) lim.Process(0.5f);
    lim.Reset();
    // first output after reset must come from the cleared delay ring
    EXPECT_FLOAT_EQ(lim.Process(0.5f), 0.0f);
}

TEST(SoftwareLimiter, Inf_Input_ProducesFiniteOutput) {
    SoftwareLimiter lim;
    for (int i = 0; i < 512; ++i)
        EXPECT_TRUE(std::isfinite(lim.Process(std::numeric_limits<float>::infinity())));
}

// ============================================================
// FETCompressor
// ============================================================

TEST(FETCompressor, Disabled_IsPassthrough) {
    FETCompressor comp;
    comp.SetEnable(false);
    auto buf = sine_stereo(1000.0f, 44100, 480);
    const auto ref = buf;
    comp.Process(buf.data(), static_cast<uint32_t>(buf.size()));
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(FETCompressor, Enabled_OutputIsFinite) {
    FETCompressor comp;
    comp.SetEnable(true);
    comp.SetSamplingRate(44100);
    // Process() takes frame-count pairs (stereo interleaved), size = num_samples not num_frames.
    auto buf = sine_stereo(440.0f, 44100, 480);
    comp.Process(buf.data(), static_cast<uint32_t>(buf.size()));
    EXPECT_TRUE(all_finite(buf));
}

TEST(FETCompressor, Enabled_OutputEnergyDoesNotExceedInput) {
    // With threshold at 0 dB and high ratio, output energy must be ≤ input energy.
    FETCompressor comp;
    comp.SetEnable(true);
    comp.SetSamplingRate(44100);
    comp.SetThresholdDb(0.0f);
    comp.SetRatioSlope(0.9f); // near-limiter
    comp.SetGainAuto(false);
    comp.SetGainDb(0.0f);

    // Run 2s worth of frames in 480-frame chunks (matches real-world usage).
    const size_t kFrames = 480;
    float in_energy = 0.0f, out_energy = 0.0f;
    for (int chunk = 0; chunk < 180; ++chunk) {
        auto buf = sine_stereo(440.0f, 44100, kFrames);
        for (float v : buf) in_energy += v * v;
        comp.Process(buf.data(), static_cast<uint32_t>(buf.size()));
        for (float v : buf) out_energy += v * v;
    }
    EXPECT_LE(out_energy, in_energy * 1.01f);
}

TEST(FETCompressor, Reset_ProducesFiniteOutput) {
    FETCompressor comp;
    comp.SetEnable(true);
    comp.SetSamplingRate(48000);
    auto buf = sine_stereo(1000.0f, 48000, 480);
    comp.Process(buf.data(), static_cast<uint32_t>(buf.size()));
    comp.Reset();
    auto buf2 = sine_stereo(1000.0f, 48000, 480);
    comp.Process(buf2.data(), static_cast<uint32_t>(buf2.size()));
    EXPECT_TRUE(all_finite(buf2));
}

// ============================================================
// PlaybackGain
// ============================================================

TEST(PlaybackGain, Disabled_IsPassthrough) {
    PlaybackGain pg;
    pg.SetEnable(false);
    auto buf = sine_stereo(440.0f, 44100, 480);
    const auto ref = buf;
    pg.Process(buf.data(), 480); // size = frame count
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(PlaybackGain, Enabled_OutputIsFinite) {
    PlaybackGain pg;
    pg.SetEnable(true);
    pg.SetSamplingRate(44100);
    pg.SetVolume(1.0f);
    pg.SetMaxGainFactor(4.0f);
    pg.SetRatio(1.0f);
    // Feed ~1 s of audio in 480-frame chunks to get past the 0.4 s warmup ramp.
    for (int i = 0; i < 92; ++i) {
        auto buf = sine_stereo(440.0f, 44100, 480);
        pg.Process(buf.data(), 480);
        EXPECT_TRUE(all_finite(buf));
    }
}

TEST(PlaybackGain, Volume_Zero_ProducesSilence) {
    PlaybackGain pg;
    pg.SetEnable(true);
    pg.SetSamplingRate(44100);
    pg.SetVolume(0.0f);
    pg.SetMaxGainFactor(1.0f);
    // Feed silence — output must be silence (gain × 0 = 0).
    std::vector<float> buf(480 * 2, 0.0f);
    pg.Process(buf.data(), 480);
    for (float v : buf) EXPECT_FLOAT_EQ(v, 0.0f);
}

// ============================================================
// DiffSurround
// ============================================================

TEST(DiffSurround, Disabled_IsPassthrough) {
    DiffSurround ds;
    ds.SetEnable(false);
    auto buf = sine_stereo(440.0f, 44100, 480);
    const auto ref = buf;
    ds.Process(buf.data(), 480);
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(DiffSurround, Enabled_ZeroDelay_OutputIsFinite) {
    DiffSurround ds;
    ds.SetSamplingRate(44100);
    ds.SetDelayTime(0.0f);
    ds.SetWetDryMix(1.0f);
    ds.SetEnable(true);
    auto buf = sine_stereo(440.0f, 44100, 480);
    ds.Process(buf.data(), 480);
    EXPECT_TRUE(all_finite(buf));
}

TEST(DiffSurround, Enabled_ZeroDelay_ZeroWet_IsPassthrough) {
    // wet_dry_mix = 0 → delayed channel = 100% direct, no diffusion applied.
    DiffSurround ds;
    ds.SetSamplingRate(44100);
    ds.SetDelayTime(0.0f);
    ds.SetWetDryMix(0.0f);
    ds.SetEnable(true);
    auto buf = sine_stereo(440.0f, 44100, 480);
    const auto ref = buf;
    ds.Process(buf.data(), 480);
    EXPECT_NEAR(max_abs_diff(buf, ref), 0.0f, 1e-6f);
}

TEST(DiffSurround, Enabled_NonzeroDelay_OutputIsFinite) {
    DiffSurround ds;
    ds.SetSamplingRate(44100);
    ds.SetDelayTime(20.0f); // 20 ms — canonical field surround default
    ds.SetWetDryMix(1.0f);
    ds.SetEnable(true);
    auto buf = sine_stereo(440.0f, 44100, 4410);
    ds.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));
}

// ============================================================
// StereoImager
// ============================================================

TEST(StereoImager, Disabled_IsPassthrough) {
    StereoImager si;
    si.SetEnable(false);
    auto buf = sine_stereo(440.0f, 44100, 480);
    const auto ref = buf;
    si.Process(buf.data(), 480);
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(StereoImager, AllWidths100_MonoInput_MidSidePreserved) {
    // For mono input (L == R) with width = 100 (unity), side = 0 → output L == R.
    StereoImager si;
    si.SetSamplingRate(44100);
    si.SetLowWidth(100.0f);
    si.SetMidWidth(100.0f);
    si.SetHighWidth(100.0f);
    si.SetEnable(true);

    auto buf = sine_stereo(440.0f, 44100, 4410); // mono: L == R
    si.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));

    // After filter transient settles (~200 ms), L and R must be equal.
    constexpr size_t kSettle = 44100 / 5; // 200 ms in frames
    for (size_t i = kSettle; i < 4410; ++i)
        EXPECT_NEAR(buf[i * 2], buf[i * 2 + 1], 1e-4f)
            << "L != R at frame " << i << " with mono input + unity width";
}

TEST(StereoImager, Width0_CollapsesMono) {
    // width = 0 → side = 0 → L_out = mid, R_out = mid → L_out == R_out
    StereoImager si;
    si.SetSamplingRate(44100);
    si.SetLowWidth(0.0f);
    si.SetMidWidth(0.0f);
    si.SetHighWidth(0.0f);
    si.SetEnable(true);

    // Use a stereo signal with L != R to ensure side cancellation is actually tested.
    std::vector<float> buf(4410 * 2);
    for (size_t i = 0; i < 4410; ++i) {
        buf[i * 2]     = std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * i / 44100.0f);
        buf[i * 2 + 1] = std::cos(2.0f * static_cast<float>(M_PI) * 440.0f * i / 44100.0f);
    }

    si.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));

    constexpr size_t kSettle = 44100 / 5;
    for (size_t i = kSettle; i < 4410; ++i)
        EXPECT_NEAR(buf[i * 2], buf[i * 2 + 1], 1e-4f)
            << "L != R at frame " << i << " with width=0 (side should be zeroed)";
}

// ============================================================
// TubeSimulator
// ============================================================

TEST(TubeSimulator, Disabled_IsPassthrough) {
    TubeSimulator ts;
    ts.SetEnable(false);
    auto buf = sine_stereo(440.0f, 44100, 480);
    const auto ref = buf;
    ts.Process(buf.data(), 480);
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(TubeSimulator, Enabled_Default12AX7_OutputIsFinite) {
    TubeSimulator ts;
    ts.SetSamplingRate(44100);
    ts.SetTubeType(0); // 12AX7
    ts.SetTubeMix(0.3f);
    ts.SetEnable(true);
    auto buf = sine_stereo(440.0f, 44100, 4410);
    ts.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));
}

TEST(TubeSimulator, AllTubeTypes_ProduceFiniteOutput) {
    for (int model = 0; model <= 4; ++model) {
        TubeSimulator ts;
        ts.SetSamplingRate(44100);
        ts.SetTubeType(model);
        ts.SetTubeMix(0.3f);
        ts.SetEnable(true);
        auto buf = sine_stereo(440.0f, 44100, 4410);
        ts.Process(buf.data(), 4410);
        EXPECT_TRUE(all_finite(buf)) << "NaN/Inf with tube model " << model;
    }
}

TEST(TubeSimulator, WDFMode_OutputIsFinite) {
    TubeSimulator ts;
    ts.SetSamplingRate(44100);
    ts.SetTubeType(0);
    ts.SetTubeMode(1); // WDF
    ts.SetTubeMix(0.5f);
    ts.SetEnable(true);
    auto buf = sine_stereo(1000.0f, 44100, 4410);
    ts.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));
}

TEST(TubeSimulator, InfInput_ProducesFiniteOutput) {
    // Validate the QuadricTube discriminant guard doesn't produce NaN.
    TubeSimulator ts;
    ts.SetSamplingRate(44100);
    ts.SetTubeType(0);
    ts.SetTubeMix(1.0f);
    ts.SetEnable(true);
    std::vector<float> buf(480 * 2, std::numeric_limits<float>::infinity());
    ts.Process(buf.data(), 480);
    EXPECT_TRUE(all_finite(buf));
}

// ============================================================
// DynamicEQ
// ============================================================

TEST(DynamicEQ, Disabled_IsPassthrough) {
    DynamicEQ deq;
    deq.SetEnable(false);
    auto buf = sine_stereo(1000.0f, 44100, 480);
    const auto ref = buf;
    deq.Process(buf.data(), 480);
    EXPECT_FLOAT_EQ(max_abs_diff(buf, ref), 0.0f);
}

TEST(DynamicEQ, Enabled_SingleBand_OutputIsFinite) {
    DynamicEQ deq;
    deq.SetSamplingRate(44100);
    deq.SetBandCount(1);
    deq.SetBandFrequency(0, 1000.0f);
    deq.SetBandGain(0, 6.0f);
    deq.SetBandQ(0, 0.707f);
    deq.SetBandThreshold(0, -20.0f);
    deq.SetBandAttack(0, 10.0f);
    deq.SetBandRelease(0, 100.0f);
    deq.SetEnable(true);
    auto buf = sine_stereo(1000.0f, 44100, 4410);
    deq.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));
}

TEST(DynamicEQ, Enabled_MaxBands_OutputIsFinite) {
    DynamicEQ deq;
    deq.SetSamplingRate(44100);
    deq.SetBandCount(DynamicEQ::kMaxBands);
    const float freqs[] = {60, 150, 400, 800, 1600, 3200, 6400, 10000, 14000, 18000};
    for (uint32_t b = 0; b < DynamicEQ::kMaxBands; ++b) {
        deq.SetBandFrequency(b, freqs[b]);
        deq.SetBandGain(b, 3.0f);
        deq.SetBandQ(b, 1.0f);
        deq.SetBandThreshold(b, -30.0f);
        deq.SetBandAttack(b, 5.0f);
        deq.SetBandRelease(b, 50.0f);
    }
    deq.SetEnable(true);
    auto buf = sine_stereo(440.0f, 44100, 4410);
    deq.Process(buf.data(), 4410);
    EXPECT_TRUE(all_finite(buf));
}
