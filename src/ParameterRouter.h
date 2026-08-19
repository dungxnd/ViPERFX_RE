#pragma once

#include "essential.h"
#include "viper/ViPER.h"
#include "viper/constants.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

namespace ParameterRouter {

// Reason the DSP pipeline is currently disabled. Replaces the old
// ViperContext::DisableReason; ViperContext re-exports it via a using alias.
enum class DisableReason : int32_t {
    UNKNOWN = -1,
    NONE = 0,
    INVALID_FRAME_COUNT,
    INVALID_SAMPLING_RATE,
    INVALID_CHANNEL_COUNT,
    INVALID_FORMAT,
};

namespace detail {

// GET-query param IDs (matches the legacy Kotlin app contract).
inline constexpr int32_t kParamGetEnabled              = 1;
inline constexpr int32_t kParamGetConfigure            = 2;
inline constexpr int32_t kParamGetStreaming             = 3;
inline constexpr int32_t kParamGetSamplingRate         = 4;
inline constexpr int32_t kParamGetConvolutionKernelId  = 5;
inline constexpr int32_t kParamGetDriverVersionCode    = 6;
inline constexpr int32_t kParamGetDriverVersionName    = 7;
inline constexpr int32_t kParamGetArchitecture         = 8;

inline int32_t read_int32(std::span<const std::byte> s, size_t byte_offset = 0) noexcept {
    int32_t v;
    std::memcpy(&v, s.data() + byte_offset, sizeof(int32_t));
    return v;
}

} // namespace detail

// ---------------------------------------------------------------------------
// HandleSet
// Decode a raw EFFECT_CMD_SET_PARAM payload and dispatch it to the DSP.
// cmd_param must already be bounds-checked (TotalSize() <= cmd_size).
// On success, writes 0 into *reply_data and returns void.
// On failure, returns a negative errno code.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::expected<void, int32_t> HandleSet(
    uint32_t cmd_size,
    const effect_param_t* cmd_param,
    void* reply_data,
    ViPER& dsp
) noexcept {
    if (constexpr uint32_t min_cmd_size = sizeof(effect_param_t) + sizeof(int32_t);
        cmd_size < min_cmd_size) {
        return std::unexpected(-EINVAL);
    }

    // Security: validate the full param+value payload fits in cmd_size before any access.
    if (cmd_size < cmd_param->TotalSize()) {
        return std::unexpected(-EINVAL);
    }

    *static_cast<int32_t*>(reply_data) = 0;

    // ParamBytes() / ValueBytes() use the canonical Align4(psize) offset from essential.h.
    const std::span<const std::byte> param_bytes = cmd_param->ParamBytes();
    const std::span<const std::byte> value_bytes = cmd_param->ValueBytes();

    const int32_t param = detail::read_int32(param_bytes);
    switch (value_bytes.size()) {
        case sizeof(int32_t): {
            dsp.DispatchRawParam(param, detail::read_int32(value_bytes), 0, 0, 0, nullptr);
            return {};
        }
        case sizeof(int32_t) * 2: {
            dsp.DispatchRawParam(
                param,
                detail::read_int32(value_bytes),
                detail::read_int32(value_bytes, sizeof(int32_t)),
                0, 0, nullptr
            );
            return {};
        }
        case sizeof(int32_t) * 3: {
            dsp.DispatchRawParam(
                param,
                detail::read_int32(value_bytes),
                detail::read_int32(value_bytes, sizeof(int32_t)),
                detail::read_int32(value_bytes, sizeof(int32_t) * 2),
                0, nullptr
            );
            return {};
        }
        case 256:
        case 1024: {
            if (value_bytes.size() < sizeof(uint32_t)) return std::unexpected(-EINVAL);
            uint32_t arr_size;
            std::memcpy(&arr_size, value_bytes.data(), sizeof(uint32_t));
            // Bounds check: arr_size must not exceed the actual payload that follows
            // the leading uint32_t header.  A corrupted or malicious packet could
            // carry an arr_size > remaining bytes, causing the DSP to read garbage.
            if (arr_size > value_bytes.size() - sizeof(uint32_t)) {
                return std::unexpected(-EINVAL);
            }
            // const_cast required: DispatchRawParam takes signed char* (non-const legacy API).
            // The data is not modified; const_cast is the minimal necessary concession.
            auto* arr = const_cast<signed char*>(
                reinterpret_cast<const signed char*>(value_bytes.data() + sizeof(uint32_t))
            );
            dsp.DispatchRawParam(param, 0, 0, 0, arr_size, arr);
            return {};
        }
        case 8192: {
            if (value_bytes.size() < sizeof(int32_t) + sizeof(uint32_t)) {
                return std::unexpected(-EINVAL);
            }
            const int32_t value1 = detail::read_int32(value_bytes);
            uint32_t arr_size;
            std::memcpy(&arr_size, value_bytes.data() + sizeof(int32_t), sizeof(uint32_t));
            // Same bounds check as the 256/1024 cases above.
            if (arr_size > value_bytes.size() - sizeof(int32_t) - sizeof(uint32_t)) {
                return std::unexpected(-EINVAL);
            }
            auto* arr = const_cast<signed char*>(
                reinterpret_cast<const signed char*>(
                    value_bytes.data() + sizeof(int32_t) + sizeof(uint32_t))
            );
            dsp.DispatchRawParam(param, value1, 0, 0, arr_size, arr);
            return {};
        }
        default: {
            return std::unexpected(-EINVAL);
        }
    }
}

// ---------------------------------------------------------------------------
// HandleGet
// Handle an EFFECT_CMD_GET_PARAM query; writes the result into reply_param.
// Returns the total reply bytes written, or a negative errno on failure.
// last_streaming_frames is an in/out state variable for kParamGetStreaming.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::expected<uint32_t, int32_t> HandleGet(
    uint32_t cmd_size,
    const effect_param_t* cmd_param,
    effect_param_t* reply_param,
    uint32_t reply_size_limit,
    const ViPER& dsp,
    bool is_enabled,
    DisableReason disable_reason,
    uint64_t& last_streaming_frames  // in/out for kParamGetStreaming
) noexcept {
    if (cmd_size < sizeof(effect_param_t) + cmd_param->psize
        || reply_size_limit < sizeof(effect_param_t) + cmd_param->psize) {
        return std::unexpected(-EINVAL);
    }

    // Delegate alignment to the canonical method on effect_param_t (essential.h).
    const uint32_t offset = cmd_param->ValueOffset();

    std::memcpy(reply_param, cmd_param, sizeof(effect_param_t) + cmd_param->psize);

    // Read the query key from the parameter bytes (psize == sizeof(uint32_t) for GET queries).
    uint32_t query;
    std::memcpy(&query, cmd_param->ParamBytes().data(), sizeof(uint32_t));

    // write_value: takes const void* so callers pass &value directly — no cast boilerplate.
    auto write_value = [&](const void* value, uint32_t vsize) noexcept -> uint32_t {
        reply_param->status = 0;
        reply_param->vsize = vsize;
        std::memcpy(reply_param->data + offset, value, vsize);
        // Correct HAL size: effect_param_t header + Align4(psize) + vsize.
        return sizeof(effect_param_t) + offset + reply_param->vsize;
    };

    switch (query) {
        case detail::kParamGetEnabled: {
            const int32_t value = is_enabled ? 1 : 0;
            return write_value(&value, sizeof(int32_t));
        }
        case detail::kParamGetConfigure: {
            const int32_t value = (disable_reason == DisableReason::NONE) ? 1 : 0;
            return write_value(&value, sizeof(int32_t));
        }
        case detail::kParamGetStreaming: {
            const uint64_t frames = dsp.GetProcessedFrames();
            const int32_t is_processing = (frames != last_streaming_frames && frames > 0) ? 1 : 0;
            last_streaming_frames = frames;
            return write_value(&is_processing, sizeof(int32_t));
        }
        case detail::kParamGetSamplingRate: {
            const uint32_t value = dsp.GetSamplingRate();
            return write_value(&value, sizeof(uint32_t));
        }
        case detail::kParamGetConvolutionKernelId: {
            const uint32_t value = dsp.GetConvolverKernelID();
            return write_value(&value, sizeof(uint32_t));
        }
        case detail::kParamGetDriverVersionCode: {
            const int32_t value = VERSION_CODE;
            return write_value(&value, sizeof(int32_t));
        }
        case detail::kParamGetDriverVersionName: {
            return write_value(VERSION_NAME, static_cast<uint32_t>(strlen(VERSION_NAME)));
        }
        case detail::kParamGetArchitecture: {
            return write_value(VIPER_ARCHITECTURE, sizeof(VIPER_ARCHITECTURE) - 1);
        }
        default: {
            return std::unexpected(-EINVAL);
        }
    }
}

} // namespace ParameterRouter
