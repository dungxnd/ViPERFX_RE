// Unit tests for FormatConverter — stateless PCM <-> float conversion.
// These tests run on the host; no Android HAL or NDK required.

#include "FormatConverter.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <numeric>

// ============================================================
// Helpers
// ============================================================

// Build a minimal audio_buffer_t pointing at caller-provided storage.
template <typename T>
static audio_buffer_t make_buffer(T* ptr, size_t frame_count) noexcept {
    audio_buffer_t buf{};
    buf.frame_count = frame_count;
    if constexpr (std::is_same_v<T, float>)       buf.f32 = ptr;
    else if constexpr (std::is_same_v<T, int32_t>) buf.s32 = ptr;
    else if constexpr (std::is_same_v<T, int16_t>) buf.s16 = ptr;
    return buf;
}

// ============================================================
// ToFloat — PCM 16-bit
// ============================================================

TEST(FormatConverter_ToFloat, Pcm16_Zero) {
    std::array<int16_t, 4> src{0, 0, 0, 0};
    std::array<float, 4>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 2);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_16_BIT);
    for (float v : dst) EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(FormatConverter_ToFloat, Pcm16_MaxPositive) {
    // INT16_MAX → slightly below 1.0f (due to +1 denominator trick)
    std::array<int16_t, 2> src{32767, 32767};
    std::array<float, 2>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 1);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_16_BIT);
    // Expected: 32767 / 32768 ≈ 0.99997
    EXPECT_NEAR(dst[0], 32767.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(dst[1], 32767.0f / 32768.0f, 1e-5f);
}

TEST(FormatConverter_ToFloat, Pcm16_MinNegative) {
    // INT16_MIN → clamped to -1.0f
    std::array<int16_t, 2> src{-32768, -32768};
    std::array<float, 2>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 1);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_16_BIT);
    // -32768 / 32768 = -1.0; clamp(-1.0, -1, 1) = -1.0
    EXPECT_FLOAT_EQ(dst[0], -1.0f);
}

TEST(FormatConverter_ToFloat, Pcm16_Stereo_Roundtrip_Check) {
    // Known stereo frame: L=16384 (0.5), R=-16384 (-0.5)
    std::array<int16_t, 2> src{16384, -16384};
    std::array<float, 2>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 1);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_16_BIT);
    EXPECT_NEAR(dst[0],  16384.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(dst[1], -16384.0f / 32768.0f, 1e-5f);
}

// ============================================================
// ToFloat — PCM 32-bit
// ============================================================

TEST(FormatConverter_ToFloat, Pcm32_MaxPositive) {
    std::array<int32_t, 2> src{2147483647, 0};
    std::array<float, 2>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 1);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_32_BIT);
    // INT32_MAX / (INT32_MAX+1) ≈ 1.0
    EXPECT_NEAR(dst[0], 1.0f, 5e-7f);
    EXPECT_FLOAT_EQ(dst[1], 0.0f);
}

TEST(FormatConverter_ToFloat, Pcm32_MinNegative) {
    std::array<int32_t, 2> src{-2147483648LL, 0};
    std::array<float, 2>   dst{};
    audio_buffer_t buf = make_buffer(src.data(), 1);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_32_BIT);
    EXPECT_FLOAT_EQ(dst[0], -1.0f);
}

// ============================================================
// ToFloat — PCM Float passthrough
// ============================================================

TEST(FormatConverter_ToFloat, Float_Passthrough) {
    std::array<float, 4> src{-0.5f, 0.25f, 0.0f, 1.0f};
    std::array<float, 4> dst{};
    audio_buffer_t buf = make_buffer(src.data(), 2);
    FormatConverter::ToFloat(dst, buf, AUDIO_FORMAT_PCM_FLOAT);
    for (size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(dst[i], src[i]);
}

TEST(FormatConverter_ToFloat, UnknownFormat_DoesNotModifyDst) {
    std::array<float, 2> dst{99.0f, 88.0f};
    // src pointer unused for unknown format
    float dummy[2] = {1.0f, 2.0f};
    audio_buffer_t buf = make_buffer(dummy, 1);
    // format 0xFF is not a valid AUDIO_FORMAT — the switch default is a no-op
    FormatConverter::ToFloat(dst, buf, 0xFF);
    EXPECT_FLOAT_EQ(dst[0], 99.0f);
    EXPECT_FLOAT_EQ(dst[1], 88.0f);
}

// ============================================================
// FromFloat — PCM 16-bit (overwrite)
// ============================================================

TEST(FormatConverter_FromFloat, Pcm16_Overwrite_MaxPositive) {
    std::array<float, 2>   src{1.0f, -1.0f};
    std::array<int16_t, 2> dst{};
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_16_BIT, false);
    EXPECT_EQ(dst[0], static_cast<int16_t>(32767));
    EXPECT_EQ(dst[1], static_cast<int16_t>(-32767));
}

TEST(FormatConverter_FromFloat, Pcm16_Overwrite_Zero) {
    std::array<float, 4>   src{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<int16_t, 4> dst{111, 222, 333, 444};
    audio_buffer_t buf = make_buffer(dst.data(), 2);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_16_BIT, false);
    for (auto v : dst) EXPECT_EQ(v, 0);
}

TEST(FormatConverter_FromFloat, Pcm16_Clamp_Over_Range) {
    // Input > 1.0f must be clamped
    std::array<float, 2>   src{2.5f, -3.0f};
    std::array<int16_t, 2> dst{};
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_16_BIT, false);
    EXPECT_EQ(dst[0], 32767);
    EXPECT_EQ(dst[1], -32767);  // clamped to ±max
}

TEST(FormatConverter_FromFloat, Pcm16_Accumulate) {
    // Existing dst = 10000 (max saturates at 32767)
    std::array<float, 2>   src{0.5f, 0.5f};
    std::array<int16_t, 2> dst{16384, 16384};
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_16_BIT, true);
    // 16384 + round(0.5 * 32767) = 16384 + 16384 = 32768 → clamp to 32767
    EXPECT_EQ(dst[0], 32767);
}

// ============================================================
// FromFloat — PCM 32-bit (overwrite + accumulate)
// ============================================================

TEST(FormatConverter_FromFloat, Pcm32_Overwrite) {
    std::array<float, 2>   src{1.0f, -1.0f};
    std::array<int32_t, 2> dst{};
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_32_BIT, false);
    EXPECT_EQ(dst[0], std::numeric_limits<int32_t>::max());
    EXPECT_EQ(dst[1], -std::numeric_limits<int32_t>::max());
}

TEST(FormatConverter_FromFloat, Pcm32_Accumulate_Saturates) {
    // max + 1 should saturate at INT32_MAX
    std::array<float, 2>   src{0.5f, 0.5f};
    std::array<int32_t, 2> dst{1073741823, 1073741823}; // INT32_MAX/2
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_32_BIT, true);
    // 1073741823 + round(0.5 * INT32_MAX) ≈ INT32_MAX  — must clamp
    EXPECT_LE(dst[0], std::numeric_limits<int32_t>::max());
    EXPECT_GE(dst[0], 0);
}

// ============================================================
// FromFloat — Float passthrough (overwrite + accumulate)
// ============================================================

TEST(FormatConverter_FromFloat, Float_Overwrite) {
    std::array<float, 4> src{0.1f, -0.2f, 0.3f, -0.4f};
    std::array<float, 4> dst{99.0f, 99.0f, 99.0f, 99.0f};
    audio_buffer_t buf = make_buffer(dst.data(), 2);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_FLOAT, false);
    for (size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(dst[i], src[i]);
}

TEST(FormatConverter_FromFloat, Float_Accumulate_Clamp) {
    std::array<float, 2> src{0.8f, 0.8f};
    std::array<float, 2> dst{0.5f, -0.5f};
    audio_buffer_t buf = make_buffer(dst.data(), 1);
    FormatConverter::FromFloat(buf, src, AUDIO_FORMAT_PCM_FLOAT, true);
    EXPECT_FLOAT_EQ(dst[0], 1.0f);   // 0.5 + 0.8 = 1.3 → clamped to 1.0
    EXPECT_FLOAT_EQ(dst[1], 0.3f);   // -0.5 + 0.8 = 0.3 (within range)
}

// ============================================================
// Symmetry: ToFloat → FromFloat roundtrip (16-bit)
// ============================================================

TEST(FormatConverter_Roundtrip, Pcm16_LossIsMinimal) {
    // For typical audio values the roundtrip loss must be < 2 LSB.
    constexpr int16_t input = 12345;
    std::array<int16_t, 2> pcm_in{input, input};
    std::array<float, 2>   floats{};
    std::array<int16_t, 2> pcm_out{};

    audio_buffer_t buf_in  = make_buffer(pcm_in.data(), 1);
    audio_buffer_t buf_out = make_buffer(pcm_out.data(), 1);

    FormatConverter::ToFloat(floats, buf_in, AUDIO_FORMAT_PCM_16_BIT);
    FormatConverter::FromFloat(buf_out, floats, AUDIO_FORMAT_PCM_16_BIT, false);

    EXPECT_NEAR(pcm_out[0], input, 2);
}

TEST(FormatConverter_Roundtrip, Float_IsLossless) {
    std::array<float, 4> src{0.1f, -0.3f, 0.7f, -0.9f};
    std::array<float, 4> dst1{};
    std::array<float, 4> dst2{};

    audio_buffer_t b1 = make_buffer(src.data(), 2);
    audio_buffer_t b2 = make_buffer(dst2.data(), 2);

    FormatConverter::ToFloat(dst1, b1, AUDIO_FORMAT_PCM_FLOAT);
    FormatConverter::FromFloat(b2, dst1, AUDIO_FORMAT_PCM_FLOAT, false);
    for (size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(dst2[i], src[i]);
}
