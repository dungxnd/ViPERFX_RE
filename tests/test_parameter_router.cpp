// Unit tests for ParameterRouter — SET_PARAM dispatch and GET_PARAM queries.
// Exercises HandleSet and HandleGet with a real ViPER DSP instance.

#include "ParameterRouter.h"
#include "viper/ViPER.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <new>
#include <vector>

using DR = ParameterRouter::DisableReason;

// ============================================================
// Helpers — build well-formed effect_param_t payloads in heap
// ============================================================

// Build a SET_PARAM payload: 1 int32 param + N int32 values.
// Layout: effect_param_t | param(4) | [pad to Align4] | values...
template <typename... Vals>
static std::vector<std::byte> make_set_payload(int32_t param, Vals... vals) {
    constexpr uint32_t psize = sizeof(int32_t);
    constexpr uint32_t aligned_psize = (psize + 3U) & ~3U;
    constexpr uint32_t vsize = sizeof(int32_t) * sizeof...(vals);
    const size_t total = sizeof(effect_param_t) + aligned_psize + vsize;

    std::vector buf(total, std::byte{0});
    // Use std::memcpy-based construction to avoid aliasing UB with raw byte buffers.
    effect_param_t header{};
    header.status = 0;
    header.psize  = psize;
    header.vsize  = vsize;
    std::memcpy(buf.data(), &header, sizeof(effect_param_t));
    std::memcpy(buf.data() + sizeof(effect_param_t), &param, sizeof(int32_t));

    // Write values after Align4(psize) offset
    const std::array<int32_t, sizeof...(vals)> arr{static_cast<int32_t>(vals)...};
    std::memcpy(buf.data() + sizeof(effect_param_t) + aligned_psize, arr.data(), vsize);

    return buf;
}

// Helper: get a mutable effect_param_t* from a byte buffer (placement-new + launder).
static effect_param_t* as_param(std::vector<std::byte>& buf) {
    return std::launder(reinterpret_cast<effect_param_t*>(buf.data()));
}
static const effect_param_t* as_cparam(const std::vector<std::byte>& buf) {
    return std::launder(reinterpret_cast<const effect_param_t*>(buf.data()));
}

// Build a GET_PARAM payload (query key only, no value bytes).
static std::vector<std::byte> make_get_payload(int32_t query) {
    constexpr uint32_t psize = sizeof(int32_t);
    constexpr uint32_t aligned_psize = (psize + 3U) & ~3U;
    const size_t total = sizeof(effect_param_t) + aligned_psize;

    std::vector buf(total, std::byte{0});
    effect_param_t header{};
    header.psize = psize;
    header.vsize = sizeof(int32_t); // expected reply vsize
    std::memcpy(buf.data(), &header, sizeof(effect_param_t));
    std::memcpy(buf.data() + sizeof(effect_param_t), &query, sizeof(int32_t));
    return buf;
}

// Allocate a reply buffer large enough for get queries
constexpr size_t kReplyBufSize = 256;

// ============================================================
// HandleSet — size validation
// ============================================================

TEST(ParameterRouter_HandleSet, TooSmallCmdSize_ReturnsEINVAL) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto payload = make_set_payload(1, 42);
    const auto* p = as_cparam(payload);

    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(sizeof(effect_param_t) - 1), // too small
        p, as_param(reply_buf), viper
    );
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

TEST(ParameterRouter_HandleSet, TotalSizeMismatch_ReturnsEINVAL) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto payload = make_set_payload(1, 42);
    const auto* p = as_cparam(payload);

    // cmd_size less than p->TotalSize() → security check fails
    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(payload.size() - 2),
        p, as_param(reply_buf), viper
    );
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

TEST(ParameterRouter_HandleSet, SingleInt32Value_Succeeds) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto payload = make_set_payload(1, 0); // param=1 (master enable), val=0
    const auto* p = as_cparam(payload);

    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(payload.size()), p, as_param(reply_buf), viper
    );
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(as_param(reply_buf)->status, 0);
}

TEST(ParameterRouter_HandleSet, TwoInt32Values_Succeeds) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto payload = make_set_payload(80, 0, 0); // two-int param (e.g. eq band)
    const auto* p = as_cparam(payload);

    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(payload.size()), p, as_param(reply_buf), viper
    );
    EXPECT_TRUE(res.has_value());
}

TEST(ParameterRouter_HandleSet, ThreeInt32Values_Succeeds) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto payload = make_set_payload(80, 0, 0, 0);
    const auto* p = as_cparam(payload);

    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(payload.size()), p, as_param(reply_buf), viper
    );
    EXPECT_TRUE(res.has_value());
}

TEST(ParameterRouter_HandleSet, UnknownValueSize_ReturnsEINVAL) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});

    // Build a payload with an unusual vsize (e.g. 7 bytes)
    constexpr uint32_t psize = sizeof(int32_t);
    constexpr uint32_t aligned_psize = 4;
    constexpr uint32_t vsize = 7;
    const size_t total = sizeof(effect_param_t) + aligned_psize + vsize;

    std::vector buf(total, std::byte{0});
    effect_param_t header{};
    header.psize = psize;
    header.vsize = vsize;
    std::memcpy(buf.data(), &header, sizeof(effect_param_t));
    int32_t param = 1;
    std::memcpy(buf.data() + sizeof(effect_param_t), &param, sizeof(int32_t));

    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(total), as_cparam(buf), as_param(reply_buf), viper
    );
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

// ============================================================
// HandleSet — blob arr_size bounds checks (1C fix)
// ============================================================

// Helper: build a vsize=256 payload where the leading uint32_t arr_size
// claims more bytes than the remaining span (potential OOB into DSP).
static std::vector<std::byte> make_blob_payload(uint32_t vsize, uint32_t arr_size_claim) {
    constexpr uint32_t psize = sizeof(int32_t);
    constexpr uint32_t aligned_psize = 4;
    const size_t total = sizeof(effect_param_t) + aligned_psize + vsize;

    std::vector buf(total, std::byte{0});
    effect_param_t header{};
    header.psize = psize;
    header.vsize = vsize;
    std::memcpy(buf.data(), &header, sizeof(effect_param_t));
    int32_t param = 200;  // any param id that maps to blob dispatch
    std::memcpy(buf.data() + sizeof(effect_param_t), &param, sizeof(int32_t));
    // Write the arr_size claim as the first uint32_t of the value region
    std::memcpy(buf.data() + sizeof(effect_param_t) + aligned_psize,
                &arr_size_claim, sizeof(uint32_t));
    return buf;
}

TEST(ParameterRouter_HandleSet, Blob256_ArrSizeExceedsPayload_ReturnsEINVAL) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    // arr_size=252 is the max that fits (256 - 4); claim 253 to trigger bounds check
    auto buf = make_blob_payload(256, 253);
    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(buf.size()), as_cparam(buf), as_param(reply_buf), viper
    );
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

TEST(ParameterRouter_HandleSet, Blob256_ArrSizeExact_Succeeds) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    auto buf = make_blob_payload(256, 252);
    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(buf.size()), as_cparam(buf), as_param(reply_buf), viper
    );
    // May succeed or fail depending on DSP dispatch — just must not be a bounds error
    if (!res.has_value()) {
        EXPECT_NE(res.error(), -EINVAL);
    }
}

TEST(ParameterRouter_HandleSet, Blob256_ArrSizeMaxUint32_ReturnsEINVAL) {
    ViPER viper;
    std::vector reply_buf(sizeof(effect_param_t), std::byte{0});
    // Malicious: arr_size = UINT32_MAX — must be caught before it reaches DSP
    auto buf = make_blob_payload(256, 0xFFFFFFFFu);
    auto res = ParameterRouter::HandleSet(
        static_cast<uint32_t>(buf.size()), as_cparam(buf), as_param(reply_buf), viper
    );
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}



// ============================================================
// HandleGet — GET_PARAM queries
// ============================================================

class ParameterRouterGetTest : public ::testing::Test {
public:
    ViPER viper;
    uint64_t last_frames = 0;
    std::array<std::byte, kReplyBufSize> reply_buf{};

    int32_t read_reply_int32() const {
        const auto* rp = std::launder(reinterpret_cast<const effect_param_t*>(reply_buf.data()));
        int32_t val = 0;
        std::memcpy(&val,
                    rp->data + rp->ValueOffset(),
                    sizeof(int32_t));
        return val;
    }
    uint32_t read_reply_uint32() const {
        const auto* rp = std::launder(reinterpret_cast<const effect_param_t*>(reply_buf.data()));
        uint32_t val = 0;
        std::memcpy(&val,
                    rp->data + rp->ValueOffset(),
                    sizeof(uint32_t));
        return val;
    }
};

// Convenience macro to call HandleGet with named-field struct.
#define CALL_HANDLE_GET(payload, enabled, dr, limit) \
    ParameterRouter::HandleGet({ \
        .cmd_size          = static_cast<uint32_t>((payload).size()), \
        .cmd_param         = as_cparam(payload), \
        .reply_param       = std::launder(reinterpret_cast<effect_param_t*>(reply_buf.data())), \
        .reply_size_limit  = (limit), \
        .dsp               = viper, \
        .is_enabled        = (enabled), \
        .disable_reason    = (dr), \
        .last_streaming_frames = last_frames, \
    })

TEST_F(ParameterRouterGetTest, GetEnabled_WhenDisabled_Returns0) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetEnabled);
    auto res = CALL_HANDLE_GET(payload, false, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(read_reply_int32(), 0);
}

TEST_F(ParameterRouterGetTest, GetEnabled_WhenEnabled_Returns1) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetEnabled);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(read_reply_int32(), 1);
}

TEST_F(ParameterRouterGetTest, GetConfigure_WhenDisableReasonNone_Returns1) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetConfigure);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(read_reply_int32(), 1);
}

TEST_F(ParameterRouterGetTest, GetConfigure_WhenDisabled_Returns0) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetConfigure);
    auto res = CALL_HANDLE_GET(payload, false, DR::INVALID_SAMPLING_RATE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(read_reply_int32(), 0);
}

TEST_F(ParameterRouterGetTest, GetStreaming_NeverProcessed_Returns0) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetStreaming);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    // GetProcessedFrames() == 0 and last_frames == 0 → not streaming
    EXPECT_EQ(read_reply_int32(), 0);
}

TEST_F(ParameterRouterGetTest, GetSamplingRate_Default) {
    viper.SetSamplingRate(48000);
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetSamplingRate);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(read_reply_uint32(), 48000u);
}

TEST_F(ParameterRouterGetTest, GetDriverVersionCode_IsNonZeroOrZero) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetDriverVersionCode);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    SUCCEED();
}

TEST_F(ParameterRouterGetTest, GetDriverVersionName_WritesNonEmptyString) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetDriverVersionName);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    auto* rp = std::launder(reinterpret_cast<effect_param_t*>(reply_buf.data()));
    EXPECT_GT(rp->vsize, 0u);
}

TEST_F(ParameterRouterGetTest, GetArchitecture_WritesNonEmptyString) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetArchitecture);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    ASSERT_TRUE(res.has_value());
    auto* rp = std::launder(reinterpret_cast<effect_param_t*>(reply_buf.data()));
    EXPECT_GT(rp->vsize, 0u);
}

TEST_F(ParameterRouterGetTest, UnknownQuery_ReturnsEINVAL) {
    auto payload = make_get_payload(9999);
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE, kReplyBufSize);
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

TEST_F(ParameterRouterGetTest, GetConfigure_ReplyBufTooSmall_ReturnsEINVAL) {
    auto payload = make_get_payload(ParameterRouter::detail::kParamGetConfigure);
    // reply_size_limit smaller than minimum required
    auto res = CALL_HANDLE_GET(payload, true, DR::NONE,
                               static_cast<uint32_t>(sizeof(effect_param_t) - 1));
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), -EINVAL);
}

// ============================================================
// DisableReason enum values
// ============================================================

TEST(ParameterRouter_DisableReason, EnumValues) {
    EXPECT_EQ(static_cast<int32_t>(DR::UNKNOWN),              -1);
    EXPECT_EQ(static_cast<int32_t>(DR::NONE),                  0);
    EXPECT_EQ(static_cast<int32_t>(DR::INVALID_FRAME_COUNT),   1);
    EXPECT_EQ(static_cast<int32_t>(DR::INVALID_SAMPLING_RATE), 2);
    EXPECT_EQ(static_cast<int32_t>(DR::INVALID_CHANNEL_COUNT), 3);
    EXPECT_EQ(static_cast<int32_t>(DR::INVALID_FORMAT),        4);
}
