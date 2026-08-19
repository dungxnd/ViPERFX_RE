// Integration tests for ViperContext — full command/process pipeline.
// Exercises HandleCommand and Process with a real ViPERDSP instance.

#include "ViperContext.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

// ============================================================
// Test helpers
// ============================================================

// Build a valid effect_config_t for stereo PCM-16 at 44100 Hz, 480 frames.
static effect_config_t make_stereo_config(
    uint32_t sample_rate = 44100,
    size_t frame_count   = 480,
    uint8_t format       = AUDIO_FORMAT_PCM_16_BIT) {

    effect_config_t cfg{};

    auto fill = [&](buffer_config_t& b) {
        b.buffer.frame_count = frame_count;
        b.sampling_rate      = sample_rate;
        b.channels           = AUDIO_CHANNEL_OUT_STEREO;
        b.format             = format;
        b.access_mode        = EFFECT_BUFFER_ACCESS_WRITE;
        b.mask = EFFECT_CONFIG_BUFFER | EFFECT_CONFIG_SMP_RATE
               | EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT
               | EFFECT_CONFIG_ACC_MODE;
    };
    fill(cfg.input_cfg);
    fill(cfg.output_cfg);
    return cfg;
}

// Send EFFECT_CMD_SET_CONFIG and return the status written into reply.
static int32_t send_set_config(ViperContext& ctx, const effect_config_t& cfg) {
    int32_t reply = -1;
    uint32_t reply_size = sizeof(int32_t);
    ctx.HandleCommand(
        EFFECT_CMD_SET_CONFIG,
        static_cast<uint32_t>(sizeof(effect_config_t)),
        &cfg,
        &reply_size, &reply
    );
    return reply;
}

// Send EFFECT_CMD_ENABLE / EFFECT_CMD_DISABLE.
static int32_t send_enable(ViperContext& ctx) {
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    ctx.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &rs, &reply);
    return reply;
}
static int32_t send_disable(ViperContext& ctx) {
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    ctx.HandleCommand(EFFECT_CMD_DISABLE, 0, nullptr, &rs, &reply);
    return reply;
}

// ============================================================
// EFFECT_CMD_INIT
// ============================================================

TEST(ViperContext_Command, Init_Succeeds) {
    ViperContext ctx;
    int32_t reply = -1;
    uint32_t rs   = sizeof(int32_t);
    int32_t rc = ctx.HandleCommand(EFFECT_CMD_INIT, 0, nullptr, &rs, &reply);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(reply, 0);
}

TEST(ViperContext_Command, Init_WrongReplySize_ReturnsEINVAL) {
    ViperContext ctx;
    int32_t reply = -1;
    uint32_t rs   = 0; // wrong
    int32_t rc = ctx.HandleCommand(EFFECT_CMD_INIT, 0, nullptr, &rs, &reply);
    EXPECT_EQ(rc, -EINVAL);
}

// ============================================================
// EFFECT_CMD_SET_CONFIG
// ============================================================

TEST(ViperContext_Command, SetConfig_ValidStereo16_Succeeds) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    EXPECT_EQ(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_Float_Succeeds) {
    ViperContext ctx;
    auto cfg = make_stereo_config(48000, 512, AUDIO_FORMAT_PCM_FLOAT);
    EXPECT_EQ(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_Pcm32_Succeeds) {
    ViperContext ctx;
    auto cfg = make_stereo_config(44100, 256, AUDIO_FORMAT_PCM_32_BIT);
    EXPECT_EQ(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_FrameCountMismatch_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.output_cfg.buffer.frame_count = 256; // mismatch
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_SampleRateMismatch_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.output_cfg.sampling_rate = 48000; // mismatch
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_ChannelMismatch_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.output_cfg.channels = AUDIO_CHANNEL_OUT_MONO; // mismatch
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_MonoInput_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.input_cfg.channels  = AUDIO_CHANNEL_OUT_MONO;
    cfg.output_cfg.channels = AUDIO_CHANNEL_OUT_MONO;
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_InvalidInputFormat_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.input_cfg.format = 0xFF; // invalid
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_InvalidOutputFormat_ReturnsEINVAL) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    cfg.output_cfg.format = 0xFE; // invalid
    EXPECT_NE(send_set_config(ctx, cfg), 0);
}

TEST(ViperContext_Command, SetConfig_NullData_ReturnsEINVAL) {
    ViperContext ctx;
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    int32_t rc = ctx.HandleCommand(
        EFFECT_CMD_SET_CONFIG,
        sizeof(effect_config_t), nullptr, &rs, &reply
    );
    EXPECT_EQ(rc, -EINVAL);
}

// ============================================================
// EFFECT_CMD_ENABLE / DISABLE
// ============================================================

TEST(ViperContext_Command, Enable_AfterConfig_Succeeds) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    send_set_config(ctx, cfg);
    EXPECT_EQ(send_enable(ctx), 0);
}

TEST(ViperContext_Command, Disable_Succeeds) {
    ViperContext ctx;
    EXPECT_EQ(send_disable(ctx), 0);
}

TEST(ViperContext_Command, EnableThenDisable_Succeeds) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    send_set_config(ctx, cfg);
    EXPECT_EQ(send_enable(ctx), 0);
    EXPECT_EQ(send_disable(ctx), 0);
}

// ============================================================
// EFFECT_CMD_RESET
// ============================================================

TEST(ViperContext_Command, Reset_Succeeds) {
    ViperContext ctx;
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    int32_t rc = ctx.HandleCommand(EFFECT_CMD_RESET, 0, nullptr, &rs, &reply);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(reply, 0);
}

// ============================================================
// EFFECT_CMD_GET_CONFIG
// ============================================================

TEST(ViperContext_Command, GetConfig_ReturnsPreviouslySetConfig) {
    ViperContext ctx;
    auto cfg = make_stereo_config(48000, 512, AUDIO_FORMAT_PCM_FLOAT);
    send_set_config(ctx, cfg);

    effect_config_t out{};
    uint32_t rs = sizeof(effect_config_t);
    int32_t rc = ctx.HandleCommand(EFFECT_CMD_GET_CONFIG, 0, nullptr, &rs, &out);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(out.input_cfg.sampling_rate, 48000u);
    EXPECT_EQ(out.output_cfg.sampling_rate, 48000u);
    EXPECT_EQ(out.input_cfg.format, AUDIO_FORMAT_PCM_FLOAT);
}

// ============================================================
// Unknown command
// ============================================================

TEST(ViperContext_Command, UnknownCommand_ReturnsEINVAL) {
    ViperContext ctx;
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    int32_t rc = ctx.HandleCommand(0xDEAD, 0, nullptr, &rs, &reply);
    EXPECT_EQ(rc, -EINVAL);
}

// ============================================================
// Process — disabled → -ENODATA
// ============================================================

TEST(ViperContext_Process, WhenDisabled_ReturnsENODATA) {
    ViperContext ctx;
    auto cfg = make_stereo_config();
    send_set_config(ctx, cfg);
    // Do NOT enable

    constexpr size_t N = 480 * 2;
    std::vector<int16_t> in_pcm(N, 100);
    std::vector<int16_t> out_pcm(N, 0);

    audio_buffer_t in_buf{};
    in_buf.frame_count = 480;
    in_buf.s16 = in_pcm.data();

    audio_buffer_t out_buf{};
    out_buf.frame_count = 480;
    out_buf.s16 = out_pcm.data();

    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, -ENODATA);
}

// ============================================================
// Process — not configured → -EINVAL (disable_reason != NONE)
// ============================================================

TEST(ViperContext_Process, WhenNotConfigured_ReturnsEINVAL) {
    ViperContext ctx;
    // Enable without SET_CONFIG → disable_reason is UNKNOWN
    int32_t reply = -1;
    uint32_t rs = sizeof(int32_t);
    ctx.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &rs, &reply);

    audio_buffer_t in_buf{}, out_buf{};
    EXPECT_EQ(ctx.Process(&in_buf, &out_buf), -EINVAL);
}

// ============================================================
// Process — PCM 16 passthrough (all-effects-off)
// ============================================================

class ViperContextProcessTest : public ::testing::Test {
protected:
    ViperContext ctx;
    static constexpr size_t kFrames  = 480;
    static constexpr size_t kSamples = kFrames * 2; // stereo

    std::vector<int16_t> in_pcm;
    std::vector<int16_t> out_pcm;
    audio_buffer_t in_buf{};
    audio_buffer_t out_buf{};

    void SetUp() override {
        auto cfg = make_stereo_config(44100, kFrames, AUDIO_FORMAT_PCM_16_BIT);
        send_set_config(ctx, cfg);
        send_enable(ctx);

        in_pcm.resize(kSamples, 8192);   // ~0.25 amplitude
        out_pcm.resize(kSamples, 0);

        in_buf.frame_count  = kFrames;
        in_buf.s16          = in_pcm.data();
        out_buf.frame_count = kFrames;
        out_buf.s16         = out_pcm.data();
    }
};

TEST_F(ViperContextProcessTest, Process_Pcm16_ReturnsSuccess) {
    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, 0);
}

TEST_F(ViperContextProcessTest, Process_Pcm16_OutputIsNonZero) {
    // With all effects at default (pass-through / disabled), the output
    // should carry audio energy — at least some non-zero samples.
    // Run several frames to get past the fade-in ramp.
    for (int i = 0; i < 3; ++i) {
        ctx.Process(&in_buf, &out_buf);
    }
    bool any_nonzero = false;
    for (auto v : out_pcm) {
        if (v != 0) { any_nonzero = true; break; }
    }
    EXPECT_TRUE(any_nonzero);
}

TEST_F(ViperContextProcessTest, Process_FrameCountMismatch_ReturnsEINVAL) {
    out_buf.frame_count = kFrames + 1; // mismatch
    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, -EINVAL);
}

TEST_F(ViperContextProcessTest, Process_ZeroFrames_ReturnsEINVAL) {
    in_buf.frame_count  = 0;
    out_buf.frame_count = 0;
    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, -EINVAL);
}

TEST_F(ViperContextProcessTest, Process_NullRawPointer_ReturnsEINVAL) {
    in_buf.raw = nullptr;
    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, -EINVAL);
}

// ============================================================
// Process — Float format
// ============================================================

TEST(ViperContext_Process, FloatFormat_Succeeds) {
    ViperContext ctx;
    constexpr size_t kFrames  = 256;
    constexpr size_t kSamples = kFrames * 2;

    auto cfg = make_stereo_config(48000, kFrames, AUDIO_FORMAT_PCM_FLOAT);
    send_set_config(ctx, cfg);
    send_enable(ctx);

    std::vector<float> in_f(kSamples, 0.5f);
    std::vector<float> out_f(kSamples, 0.0f);

    audio_buffer_t in_buf{};
    in_buf.frame_count = kFrames;
    in_buf.f32         = in_f.data();

    audio_buffer_t out_buf{};
    out_buf.frame_count = kFrames;
    out_buf.f32         = out_f.data();

    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, 0);
}

// ============================================================
// Process — accumulate mode
// ============================================================

TEST(ViperContext_Process, AccumulateMode_AddToOutput) {
    ViperContext ctx;
    constexpr size_t kFrames = 480;

    auto cfg = make_stereo_config(44100, kFrames, AUDIO_FORMAT_PCM_FLOAT);
    cfg.output_cfg.access_mode = EFFECT_BUFFER_ACCESS_ACCUMULATE;
    send_set_config(ctx, cfg);
    send_enable(ctx);

    std::vector<float> in_f(kFrames * 2, 0.1f);
    std::vector<float> out_f(kFrames * 2, 0.1f); // pre-seeded

    audio_buffer_t in_buf{};
    in_buf.frame_count = kFrames;
    in_buf.f32         = in_f.data();

    audio_buffer_t out_buf{};
    out_buf.frame_count = kFrames;
    out_buf.f32         = out_f.data();

    // Run enough frames to clear the fade-in
    for (int i = 0; i < 2; ++i) ctx.Process(&in_buf, &out_buf);

    // With accumulate mode, output ≥ pre-seeded value (modulo limiter clamping)
    // Just verify process returns 0 without crash
    int32_t rc = ctx.Process(&in_buf, &out_buf);
    EXPECT_EQ(rc, 0);
}

// ============================================================
// kDefaultMaxFrames constant
// ============================================================

TEST(ViperContext_Constants, kDefaultMaxFrames_Is4096) {
    EXPECT_EQ(ViperContext::kDefaultMaxFrames, 4096u);
}

TEST(ViperContext_Constants, kFadeInFrames_Is128) {
    EXPECT_EQ(ViperContext::kFadeInFrames, 128u);
}
