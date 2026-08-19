// AIDL audio effect HAL entry point for ViPER4Android (Android 13+ / API 33+).
//
// The AIDL EffectFactory dlopen()s this .so and resolves two C symbols:
//   createEffect  – instantiate a new ViPER effect instance
//   queryEffect   – return the static Descriptor without instantiating
//
// Audio data flow (standard IEffect FMQ contract):
//   open()  → allocates DataMQ (float32 stereo) + StatusMQ, returns descriptors
//   command(START) → starts worker thread
//   worker  → reads frames from DataMQ, calls ViperContext::Process(), writes back
//   command(STOP/RESET) → stops worker thread
//
// Parameters arrive as a raw VendorExtension byte blob mapping 1:1 to the
// legacy effect_param_t payload, so the app side needs no changes.
//
// Design: implements BnEffect directly — no dependency on AOSP EffectImpl which
// requires platform headers not available in the Android NDK (r28+).

#define LOG_TAG "ViPER4Android"
#include <android/log.h>
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// NDK binder
#include <android/binder_status.h>

// AIDL-generated NDK stubs (from aidl-gen/include/)
#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>
#include <fmq/AidlMessageQueue.h>

#include "ViperContext.h"
#include "viper/ViPERParams.h"
#include "viper/constants.h"

// ---------------------------------------------------------------------------
// Convenience aliases
// ---------------------------------------------------------------------------
using aidl::android::hardware::audio::effect::BnEffect;
using aidl::android::hardware::audio::effect::CommandId;
using aidl::android::hardware::audio::effect::DefaultExtension;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::Flags;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::Parameter;
using aidl::android::hardware::audio::effect::State;
using aidl::android::hardware::audio::effect::VendorExtension;
using aidl::android::media::audio::common::AudioUuid;

using StatusMQ = ::android::AidlMessageQueue<
    IEffect::Status,
    ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>;
using DataMQ = ::android::AidlMessageQueue<
    float,
    ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>;

// ---------------------------------------------------------------------------
// Effect UUIDs
//   type : 7261676f-6d75-7369-6364-28e2fd3ac39e  (vendor/extension)
//   impl : 90380da3-8536-4744-a6a3-5731970e640f  (ViPER4Android)
// ---------------------------------------------------------------------------
// NOTE: AudioUuid::node is std::vector<uint8_t> in the AIDL NDK backend, which
// performs heap allocation even in constexpr evaluation contexts. A global
// constexpr/inline-constexpr object cannot retain a heap allocation past
// compile-time evaluation, so these must stay `const` (not `constexpr`).
const AudioUuid kTypeUuid = {
    static_cast<int32_t>(0x7261676f),
    static_cast<int16_t>(0x6d75),
    static_cast<int16_t>(0x7369),
    static_cast<int16_t>(0x6364),
    {0x28, 0xe2, 0xfd, 0x3a, 0xc3, 0x9e}
};
const AudioUuid kImplUuid = {
    static_cast<int32_t>(0x90380da3),
    static_cast<int16_t>(0x8536),
    static_cast<int16_t>(0x4744),
    static_cast<int16_t>(0xa6a3),
    {0x57, 0x31, 0x97, 0x0e, 0x64, 0x0f}
};

inline constexpr std::string_view kEffectName       = "ViPER4Android";
inline constexpr std::string_view kImplementorName  = VIPER_AUTHORS;

// ---------------------------------------------------------------------------
// ShmChannel — maps the three ConfigChannel SHM files and provides
// poll-on-change helpers for the AIDL worker loop.
//
// Layout mirrors ConfigChannel.kt / ViperParamsLayout.kt exactly:
//
//   shm_params.bin (4096 bytes):
//     [0..3]   magic  (0x534D3456)
//     [4..7]   format version  (6)
//     [8..11]  active slot index (0 or 1)
//     [12..15] update counter
//     [16..19] ViPERParams struct size
//     [20..]   slot A  (ViPERParams, 1164 bytes)
//     [20+1164..]  slot B  (ViPERParams, 1164 bytes)
//
//   shm_bulk.bin (4096 bytes):
//     DDC sub-channel  [0..2047]:
//       [8..11]  seq
//       [12..15] command  (1=DDC, 3=DDC_RESET)
//       [16..19] data size
//       [32..]   payload: int32 perRateFloats, int32 totalFloats, then floats
//     Convolver sub-channel [2048..4095]:
//       [8..11]  seq
//       [12..15] command  (2=CONVOLVER_PATH, 4=CONVOLVER_RESET)
//       [16..19] data size
//       [32..]   payload: null-terminated UTF-8 path
// ---------------------------------------------------------------------------
class ShmChannel {
public:
    // SHM layout constants (must match ConfigChannel.kt)
    static constexpr size_t kParamsShmSize  = 4096;
    static constexpr size_t kBulkShmSize    = 4096;
    static constexpr uint32_t kShmMagic    = 0x534D3456u;
    static constexpr uint32_t kFormatVer   = 6u;
    static constexpr size_t kParamsStructSize = 1164; // ViperParamsLayout.SIZE

    static constexpr int32_t kParamsActiveOffset     = 8;
    static constexpr int32_t kParamsUpdateCountOffset = 12;
    static constexpr int32_t kParamsHeaderSize        = 20;
    static constexpr int32_t kParamsSlotAOffset       = kParamsHeaderSize;
    static constexpr int32_t kParamsSlotBOffset       = kParamsHeaderSize + static_cast<int32_t>(kParamsStructSize);

    static constexpr int32_t kBulkDdcBase             = 0;
    static constexpr int32_t kBulkConvolverBase       = 2048;
    static constexpr int32_t kBulkSeqOffset           = 8;
    static constexpr int32_t kBulkCommandOffset       = 12;
    static constexpr int32_t kBulkDataSizeOffset      = 16;
    static constexpr int32_t kBulkHeaderSize          = 32;
    static constexpr int32_t kBulkCmdDdc              = 1;
    static constexpr int32_t kBulkCmdConvolverPath    = 2;
    static constexpr int32_t kBulkCmdDdcReset         = 3;
    static constexpr int32_t kBulkCmdConvolverReset   = 4;

    ShmChannel() = default;
    ~ShmChannel() { unmap(); }
    ShmChannel(const ShmChannel&)            = delete;
    ShmChannel& operator=(const ShmChannel&) = delete;

    // Open + mmap both shm files.  Non-fatal: if either file is missing the
    // ShmChannel stays disabled and pollParams/pollBulk are no-ops.
    void open() {
        paramsPtr_ = mapFile("/data/local/tmp/v4a/shm_params.bin", kParamsShmSize);
        bulkPtr_   = mapFile("/data/local/tmp/v4a/shm_bulk.bin",   kBulkShmSize);
        if (paramsPtr_) {
            ALOGD("ShmChannel: params mmap OK");
        } else {
            ALOGE("ShmChannel: params mmap failed — parameters will not be applied");
        }
        if (bulkPtr_) {
            ALOGD("ShmChannel: bulk mmap OK");
        } else {
            ALOGE("ShmChannel: bulk mmap failed — DDC/convolver will not be applied");
        }
    }

    // Retry mapping whichever files were absent at open() time.
    // Called from the worker loop on every iteration until both are mapped.
    // Once both ptrs are non-null this becomes a single null-check branch — no
    // syscalls, no log spam.
    void tryOpenMissing() noexcept {
        if (!paramsPtr_) {
            paramsPtr_ = mapFile("/data/local/tmp/v4a/shm_params.bin",
                                 kParamsShmSize, /*silent_enoent=*/true);
            if (paramsPtr_) ALOGI("ShmChannel: params mmap OK (late open)");
        }
        if (!bulkPtr_) {
            bulkPtr_ = mapFile("/data/local/tmp/v4a/shm_bulk.bin",
                               kBulkShmSize, /*silent_enoent=*/true);
            if (bulkPtr_) ALOGI("ShmChannel: bulk mmap OK (late open)");
        }
    }

    // Poll the params SHM for a new snapshot.  Returns the active-slot pointer
    // (into the mmap region) when the update counter has changed since the last
    // call, or nullptr when nothing changed / SHM unavailable.
    // The pointer is valid until the next workerLoop iteration — no copy needed;
    // ViPER::ApplyParams() reads the struct by value.
    const viper::ViPERParams* pollParams() noexcept {
        if (!paramsPtr_) return nullptr;
        const auto* base = static_cast<const uint8_t*>(paramsPtr_);

        uint32_t counter;
        std::memcpy(&counter, base + kParamsUpdateCountOffset, sizeof(counter));
        if (counter == lastParamsCounter_) return nullptr;
        lastParamsCounter_ = counter;

        uint32_t slot;
        std::memcpy(&slot, base + kParamsActiveOffset, sizeof(slot));
        const int32_t slotOff = (slot == 0) ? kParamsSlotAOffset : kParamsSlotBOffset;

        // Safety: ensure the slot fits within the mapped region
        if (static_cast<size_t>(slotOff) + kParamsStructSize > kParamsShmSize) {
            ALOGE("ShmChannel: corrupt slot offset %d", slotOff);
            return nullptr;
        }
        return reinterpret_cast<const viper::ViPERParams*>(base + slotOff);
    }

    // Poll the DDC bulk sub-channel.  Returns true and fills [out_sections44,
    // out_sections48, out_count] when a new DDC payload is available.
    // Returns false (no-op) when nothing changed.
    bool pollDdc(const viper::BiquadSection** out44, const viper::BiquadSection** out48,
                 uint32_t* out_count, bool* out_reset) noexcept {
        if (!bulkPtr_) return false;
        const auto* base = static_cast<const uint8_t*>(bulkPtr_) + kBulkDdcBase;

        int32_t seq, cmd, dataSize;
        std::memcpy(&seq,      base + kBulkSeqOffset,      sizeof(seq));
        std::memcpy(&cmd,      base + kBulkCommandOffset,  sizeof(cmd));
        std::memcpy(&dataSize, base + kBulkDataSizeOffset, sizeof(dataSize));
        if (seq == lastDdcSeq_) return false;
        lastDdcSeq_ = seq;

        if (cmd == kBulkCmdDdcReset) {
            *out_reset = true;
            return true;
        }
        if (cmd != kBulkCmdDdc) return false;
        *out_reset = false;

        // Payload layout: int32 perRateFloats, int32 totalFloats, then floats.
        // totalFloats == perRateFloats * 2  (44100 first, 48000 second)
        const auto* payload = base + kBulkHeaderSize;
        int32_t perRate, total;
        std::memcpy(&perRate, payload,                   sizeof(perRate));
        std::memcpy(&total,   payload + sizeof(int32_t), sizeof(total));
        if (perRate <= 0 || total != perRate * 2) return false;
        if (dataSize < 8 + total * static_cast<int32_t>(sizeof(float))) return false;

        const auto* floats = reinterpret_cast<const float*>(payload + 8);
        // Sections: each BiquadSection is 5 floats.
        if (perRate % 5 != 0) return false;
        *out_count = static_cast<uint32_t>(perRate / 5);
        *out44     = reinterpret_cast<const viper::BiquadSection*>(floats);
        *out48     = reinterpret_cast<const viper::BiquadSection*>(floats + perRate);
        return true;
    }

    // Poll the convolver bulk sub-channel.  Returns true and fills [out_path]
    // or sets [out_reset] when a new command is available.
    bool pollConvolver(std::string* out_path, bool* out_reset) noexcept {
        if (!bulkPtr_) return false;
        const auto* base = static_cast<const uint8_t*>(bulkPtr_) + kBulkConvolverBase;

        int32_t seq, cmd, dataSize;
        std::memcpy(&seq,      base + kBulkSeqOffset,      sizeof(seq));
        std::memcpy(&cmd,      base + kBulkCommandOffset,  sizeof(cmd));
        std::memcpy(&dataSize, base + kBulkDataSizeOffset, sizeof(dataSize));
        if (seq == lastConvolverSeq_) return false;
        lastConvolverSeq_ = seq;

        if (cmd == kBulkCmdConvolverReset) {
            *out_reset = true;
            return true;
        }
        if (cmd != kBulkCmdConvolverPath) return false;
        *out_reset = false;
        if (dataSize <= 0 ||
            static_cast<size_t>(kBulkHeaderSize + dataSize) > kBulkShmSize / 2) {
            return false;
        }
        const auto* pathBytes = base + kBulkHeaderSize;
        out_path->assign(reinterpret_cast<const char*>(pathBytes),
                         static_cast<size_t>(dataSize));
        return true;
    }

private:
    void* paramsPtr_ { nullptr };
    void* bulkPtr_   { nullptr };

    uint32_t lastParamsCounter_  { 0 };
    int32_t  lastDdcSeq_         { 0 };
    int32_t  lastConvolverSeq_   { 0 };

    // silent_enoent: if true, suppress the error log when the file simply does
    // not exist yet (used by tryOpenMissing() to avoid per-frame log spam).
    static void* mapFile(const char* path, size_t size,
                         bool silent_enoent = false) noexcept {
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (!silent_enoent || errno != ENOENT)
                ALOGE("ShmChannel: open(%s) failed: %s", path, strerror(errno));
            return nullptr;
        }
        void* ptr = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED) {
            ALOGE("ShmChannel: mmap(%s, %zu) failed: %s", path, size, strerror(errno));
            return nullptr;
        }
        // Validate magic + version in the params file.
        const auto* u32 = static_cast<const uint32_t*>(ptr);
        if (u32[0] != kShmMagic || u32[1] != kFormatVer) {
            ALOGI("ShmChannel: %s magic/version mismatch (0x%08x/%u) — will init on first write",
                  path, u32[0], u32[1]);
        }
        return ptr;
    }

    void unmap() noexcept {
        if (paramsPtr_) { ::munmap(paramsPtr_, kParamsShmSize); paramsPtr_ = nullptr; }
        if (bulkPtr_)   { ::munmap(bulkPtr_,   kBulkShmSize);   bulkPtr_   = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// ViPEREffect — direct BnEffect implementation (no EffectImpl base class)
// ---------------------------------------------------------------------------
class ViPEREffect final : public BnEffect {
public:
    ViPEREffect()  { ALOGD("ViPEREffect created"); }
    ~ViPEREffect() override { closeInternal(); ALOGD("ViPEREffect destroyed"); }

    // --- IEffect interface ---

    ndk::ScopedAStatus open(const Parameter::Common& common,
                            const std::optional<Parameter::Specific>& /*specific*/,
                            IEffect::OpenEffectReturn* ret) override {
        std::unique_lock lock(mMutex);
        if (mState != State::INIT) {
            ALOGE("open: already open");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }

        // Store config
        mSampleRate  = static_cast<uint32_t>(common.input.base.sampleRate);
        mFrameCount  = static_cast<size_t>(common.input.frameCount);

        // Initialise DSP pipeline
        effect_config_t cfg = {};
        cfg.input_cfg.sampling_rate  = mSampleRate;
        cfg.output_cfg.sampling_rate = mSampleRate;
        cfg.input_cfg.channels       = AUDIO_CHANNEL_OUT_STEREO;
        cfg.output_cfg.channels      = AUDIO_CHANNEL_OUT_STEREO;
        cfg.input_cfg.format         = AUDIO_FORMAT_PCM_FLOAT;
        cfg.output_cfg.format        = AUDIO_FORMAT_PCM_FLOAT;
        cfg.input_cfg.buffer.frame_count  = mFrameCount;
        cfg.output_cfg.buffer.frame_count = mFrameCount;
        cfg.input_cfg.mask  = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS
                            | EFFECT_CONFIG_FORMAT   | EFFECT_CONFIG_BUFFER;
        cfg.output_cfg.mask = cfg.input_cfg.mask;

        uint32_t replySize = sizeof(int32_t);
        int32_t  reply     = 0;
        (void)mContext.HandleCommand(EFFECT_CMD_INIT, sizeof(int32_t), nullptr, &replySize,
                                     reinterpret_cast<std::byte*>(&reply));
        (void)mContext.HandleCommand(EFFECT_CMD_SET_CONFIG, static_cast<uint32_t>(sizeof(cfg)),
                                     reinterpret_cast<const std::byte*>(&cfg),
                                     &replySize, reinterpret_cast<std::byte*>(&reply));

        // Allocate FMQ: stereo float frames, twice the buffer size for headroom
        const size_t kDataMQDepth   = mFrameCount * 2 /* channels */ * 2 /* headroom */;
        const size_t kStatusMQDepth = 1;

        mInputMQ  = std::make_unique<DataMQ>(kDataMQDepth, /*configureEventFlag=*/true);
        mOutputMQ = std::make_unique<DataMQ>(kDataMQDepth, /*configureEventFlag=*/false);
        mStatusMQ = std::make_unique<StatusMQ>(kStatusMQDepth, /*configureEventFlag=*/false);

        if (!mInputMQ->isValid() || !mOutputMQ->isValid() || !mStatusMQ->isValid()) {
            ALOGE(
                "open: failed to create FMQ (dataDepth=%zu statusDepth=%zu "
                "input=%d output=%d status=%d)",
                kDataMQDepth,
                kStatusMQDepth,
                mInputMQ && mInputMQ->isValid(),
                mOutputMQ && mOutputMQ->isValid(),
                mStatusMQ && mStatusMQ->isValid()
            );
            cleanupQueuesLocked();
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }

        // Create EventFlag once here; reused by workerLoop for every audio frame.
        // Creating it inside the RT loop causes system calls and heap alloc per frame.
        if (auto* efWord = mInputMQ->getEventFlagWord()) {
            android::hardware::EventFlag::createEventFlag(efWord, &mEventFlag);
        }

        // Pre-allocate working buffer to maximum capacity to prevent RT realloc.
        mWorkerBuf.reserve(kDataMQDepth);

        ret->statusMQ    = mStatusMQ->dupeDesc();
        ret->inputDataMQ = mInputMQ->dupeDesc();
        ret->outputDataMQ = mOutputMQ->dupeDesc();

        // Map SHM parameter/bulk files.  Non-fatal: worker polling is no-op if
        // either file is absent.  The app (ConfigChannel.kt) creates them on first
        // writeFullState() call — they may not exist yet if the effect opened before
        // the app wrote its first state snapshot.
        mShm.open();

        mState = State::IDLE;
        ALOGD("open: sr=%u frames=%zu", mSampleRate, mFrameCount);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus reopen(IEffect::OpenEffectReturn* ret) override {
        std::unique_lock lock(mMutex);
        if (!mInputMQ || !mOutputMQ || !mStatusMQ) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        ret->statusMQ    = mStatusMQ->dupeDesc();
        ret->inputDataMQ = mInputMQ->dupeDesc();
        ret->outputDataMQ = mOutputMQ->dupeDesc();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus close() override {
        closeInternal();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getState(State* state) override {
        std::unique_lock lock(mMutex);
        *state = mState;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus command(CommandId cmd) override {
        std::unique_lock lock(mMutex);
        uint32_t replySize = sizeof(int32_t);
        auto reply_bytes = std::array<std::byte, sizeof(int32_t)>{};
        switch (cmd) {
            case CommandId::START:
                if (mState == State::IDLE || mState == State::DRAINING) {
                    (void)mContext.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &replySize,
                                                 reply_bytes.data());
                    startWorkerLocked();
                    mState = State::PROCESSING;
                }
                break;
            case CommandId::STOP:
                if (mState == State::PROCESSING) {
                    stopWorkerLocked(lock);
                    (void)mContext.HandleCommand(EFFECT_CMD_DISABLE, 0, nullptr, &replySize,
                                                 reply_bytes.data());
                    mState = State::IDLE;
                }
                break;
            case CommandId::RESET:
                stopWorkerLocked(lock);
                (void)mContext.HandleCommand(EFFECT_CMD_RESET, 0, nullptr, &replySize,
                                             reply_bytes.data());
                mState = State::IDLE;
                break;
            default:
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getDescriptor(Descriptor* desc) override {
        *desc = kDescriptor;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setParameter(const Parameter& param) override {
        if (param.getTag() == Parameter::specific) {
            return setParameterSpecific(param.get<Parameter::specific>());
        }
        // Common parameters (volume, etc.) — accept silently
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getParameter(const Parameter::Id& id, Parameter* param) override {
        if (id.getTag() == Parameter::Id::vendorEffectTag) {
            Parameter::Specific specific;
            auto status = getParameterSpecific(id, &specific);
            if (!status.isOk()) return status;
            param->set<Parameter::specific>(specific);
            return ndk::ScopedAStatus::ok();
        }
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    static const Descriptor kDescriptor;

private:
    // --- parameter helpers ---

    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific) {
        if (specific.getTag() != Parameter::Specific::vendorEffect) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        auto& ve = specific.get<Parameter::Specific::vendorEffect>();
        std::optional<DefaultExtension> ext;
        if (STATUS_OK != ve.extension.getParcelable(&ext) || !ext.has_value()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        std::span<const uint8_t> bytes = ext->bytes;
        if (bytes.size() < sizeof(effect_param_t)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        const auto* p = static_cast<const effect_param_t *>(static_cast<const void *>(bytes.data()));

        // Overflow-safe bounds check: compute in size_t to avoid uint32_t wrap-around.
        const size_t psize = p->psize;
        const size_t vsize = p->vsize;
        if (psize > std::numeric_limits<size_t>::max() - sizeof(effect_param_t) - vsize) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        const size_t cmdSize = sizeof(effect_param_t) + psize + vsize;
        if (cmdSize > bytes.size()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        uint32_t replySize = sizeof(int32_t);
        int32_t  reply     = 0;
        std::unique_lock lock(mMutex);
        (void)mContext.HandleCommand(EFFECT_CMD_SET_PARAM,
                                     static_cast<uint32_t>(cmdSize),
                                     reinterpret_cast<const std::byte*>(p),
                                     &replySize,
                                     reinterpret_cast<std::byte*>(&reply));
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id,
                                             Parameter::Specific* specific) {
        auto extId = id.get<Parameter::Id::vendorEffectTag>();
        std::optional<DefaultExtension> idExt;
        if (STATUS_OK != extId.extension.getParcelable(&idExt) || !idExt.has_value()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        const auto& idBytes = idExt->bytes;
        if (idBytes.size() < sizeof(effect_param_t)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        constexpr size_t kReplyBuf = 4096;
        std::vector<std::byte> outBytes(kReplyBuf);
        uint32_t replySize = static_cast<uint32_t>(kReplyBuf);
        {
            std::unique_lock lock(mMutex);
            (void)mContext.HandleCommand(EFFECT_CMD_GET_PARAM,
                                   static_cast<uint32_t>(idBytes.size()),
                                   reinterpret_cast<const std::byte*>(idBytes.data()),
                                   &replySize, outBytes.data());
        }
        outBytes.resize(replySize);

        DefaultExtension outExt;
        // Convert std::byte → uint8_t for the Parcelable API.
        outExt.bytes.resize(outBytes.size());
        for (size_t i = 0; i < outBytes.size(); ++i) {
            outExt.bytes[i] = std::to_integer<uint8_t>(outBytes[i]);
        }
        VendorExtension ve;
        if (STATUS_OK != ve.extension.setParcelable(outExt)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        specific->set<Parameter::Specific::vendorEffect>(ve);
        return ndk::ScopedAStatus::ok();
    }

    // --- worker thread ---

    void startWorkerLocked() {
        // std::jthread: RAII join + cooperative stop via stop_token; replaces
        // std::thread + std::atomic<bool> mWorkerExit.
        mWorkerThread = std::jthread([this](std::stop_token st) {
            workerLoop(std::move(st));
        });
    }

    // Must be called with mMutex held via a std::unique_lock so we can safely
    // release the lock before joining (avoids deadlock with the worker).
    void stopWorkerLocked(std::unique_lock<std::mutex>& lock) {
        if (!mWorkerThread.joinable()) return;

        // Signal cooperative stop; then issue a FUTEX_WAKE via EventFlag::wake()
        // so the worker unblocks immediately instead of waiting up to 100ms for
        // the EventFlag::wait() timeout to expire.
        mWorkerThread.request_stop();
        if (mEventFlag) {
            mEventFlag->wake(0xFFFFFFFF);
        }

        // Release mutex before joining: the worker may need the mutex to finish.
        lock.unlock();
        mWorkerThread.join();
        lock.lock();
    }

    void workerLoop(std::stop_token stopToken) {
        ALOGD("worker: started");

        // Boost to SCHED_FIFO priority 3 — same level as standard Android audio
        // HAL worker threads — to avoid preemption-induced buffer underruns under
        // heavy UI or game load. Fails gracefully (non-root builds) without abort.
        {
            struct sched_param sp{};
            sp.sched_priority = 3;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
                ALOGD("worker: SCHED_FIFO unavailable, continuing at SCHED_OTHER");
            }
        }

        constexpr size_t kChannels = 2;

        while (!stopToken.stop_requested()) {
            // mEventFlag was created once in open(); no system call here.
            if (mEventFlag) {
                uint32_t bits = 0;
                mEventFlag->wait(/*bitmask=*/0xFFFFFFFF, &bits,
                                 /*timeoutNs=*/100'000'000ULL, /*retry=*/true);
            }

            if (stopToken.stop_requested()) break;

            // ── SHM late-open (race window: app may not have written SHM yet) ─
            // ConfigChannel.kt creates the files on the first writeFullState() call.
            // If they were absent at open() time, retry here on every iteration
            // until they appear (cheap: mapFile only does work when ptr is null).
            mShm.tryOpenMissing();

            // ── SHM parameter polling ────────────────────────────────────────
            // The Android app writes ViPERParams snapshots via ConfigChannel.kt
            // (shm_params.bin, double-buffered) and DDC/convolver data via the
            // bulk channel (shm_bulk.bin).  Poll here — before reading audio
            // frames — so the DSP always uses the most recent user settings.
            // mMutex is held only around the DSP apply/process calls, not around
            // the SHM reads, to keep the lock scope minimal.
            {
                // Params snapshot
                if (const viper::ViPERParams* snap = mShm.pollParams()) {
                    std::unique_lock lock(mMutex);
                    mContext.viper().ApplyParams(*snap);
                }

                // DDC bulk channel
                {
                    const viper::BiquadSection* sec44 = nullptr;
                    const viper::BiquadSection* sec48 = nullptr;
                    uint32_t sectionCount = 0;
                    bool ddcReset = false;
                    if (mShm.pollDdc(&sec44, &sec48, &sectionCount, &ddcReset)) {
                        std::unique_lock lock(mMutex);
                        if (ddcReset) {
                            mContext.viper().LoadDdcCoefficients(nullptr, nullptr, 0);
                        } else {
                            mContext.viper().LoadDdcCoefficients(sec44, sec48, sectionCount);
                        }
                    }
                }

                // Convolver bulk channel — poll path without the lock;
                // loadKernelFromPath does file I/O and then acquires mMutex
                // itself for the final DSP apply.
                {
                    std::string kernelPath;
                    bool convolverReset = false;
                    if (mShm.pollConvolver(&kernelPath, &convolverReset)) {
                        if (convolverReset) {
                            std::unique_lock lock(mMutex);
                            mContext.viper().UnloadConvolverKernel();
                        } else {
                            loadKernelFromPath(kernelPath); // acquires mMutex internally
                        }
                    }
                }
            }
            // ── end SHM polling ──────────────────────────────────────────────

            const size_t avail = mInputMQ->availableToRead();
            if (avail == 0) continue;

            // mWorkerBuf was reserve()d in open() to mFrameCount*4; resize never
            // reallocates as long as avail stays within that capacity.
            mWorkerBuf.resize(avail);
            if (!mInputMQ->read(mWorkerBuf.data(), avail)) continue;

            const size_t frames = avail / kChannels;
            audio_buffer_t inBuf  = { .frame_count = frames, .f32 = mWorkerBuf.data() };
            audio_buffer_t outBuf = { .frame_count = frames, .f32 = mWorkerBuf.data() };
            // Hold mMutex across Process() to serialise against the SHM apply
            // calls above and setParameter() calls on the Binder thread.
            {
                std::unique_lock lock(mMutex);
                (void)mContext.Process(&inBuf, &outBuf);
            }

            // Write processed samples to output MQ
            mOutputMQ->write(mWorkerBuf.data(), avail);

            // Signal status
            IEffect::Status st = { STATUS_OK, static_cast<int32_t>(avail),
                                              static_cast<int32_t>(avail) };
            mStatusMQ->write(&st, 1);
        }
        ALOGD("worker: stopped");
    }

    // Load a convolver kernel from a staged WAV file path.
    // File I/O is done without the lock; mMutex is acquired only for the final
    // LoadConvolverKernel() call to serialise against Process().
    void loadKernelFromPath(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            ALOGE("loadKernelFromPath: open(%s) failed: %s", path.c_str(), strerror(errno));
            return;
        }
        struct stat st{};
        if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
            ALOGE("loadKernelFromPath: fstat(%s) failed", path.c_str());
            ::close(fd);
            return;
        }
        std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
        ssize_t rd = ::read(fd, data.data(), data.size());
        ::close(fd);
        if (rd != static_cast<ssize_t>(data.size())) {
            ALOGE("loadKernelFromPath: read incomplete %zd/%zu", rd, data.size());
            return;
        }

        // Parse minimal WAV header: RIFF + fmt + data chunks.
        // We expect 32-bit IEEE float stereo (format 3, or any PCM we can handle).
        if (data.size() < 44) { ALOGE("loadKernelFromPath: file too small"); return; }

        uint32_t sampleRate = 0, byteRate = 0;
        uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
        uint32_t dataSize = 0;
        size_t dataOffset = 0;

        // Walk chunks after the 12-byte RIFF header.
        size_t pos = 12;
        while (pos + 8 <= data.size()) {
            uint32_t chunkId, chunkSize;
            std::memcpy(&chunkId,   data.data() + pos,     4);
            std::memcpy(&chunkSize, data.data() + pos + 4, 4);
            pos += 8;
            constexpr uint32_t kFmt  = 0x20746D66u; // 'fmt '
            constexpr uint32_t kData = 0x61746164u; // 'data'
            if (chunkId == kFmt && chunkSize >= 16) {
                std::memcpy(&audioFmt,      data.data() + pos,      2);
                std::memcpy(&channels,      data.data() + pos + 2,  2);
                std::memcpy(&sampleRate,    data.data() + pos + 4,  4);
                std::memcpy(&byteRate,      data.data() + pos + 8,  4);
                std::memcpy(&bitsPerSample, data.data() + pos + 14, 2);
            } else if (chunkId == kData) {
                dataOffset = pos;
                dataSize   = chunkSize;
                break;
            }
            pos += chunkSize + (chunkSize & 1); // word-align
        }

        if (dataOffset == 0 || channels == 0 || sampleRate == 0) {
            ALOGE("loadKernelFromPath: invalid WAV structure in %s", path.c_str());
            return;
        }

        // Convert samples to float.
        std::vector<float> samples;
        const uint32_t frameCount = dataSize / (channels * (bitsPerSample / 8));
        samples.resize(static_cast<size_t>(frameCount) * channels);
        const uint8_t* src = data.data() + dataOffset;

        if (audioFmt == 3 && bitsPerSample == 32) {
            // IEEE float — direct copy
            std::memcpy(samples.data(), src, samples.size() * 4);
        } else if (audioFmt == 1 && bitsPerSample == 16) {
            // PCM 16-bit → float
            for (size_t i = 0; i < samples.size(); ++i) {
                int16_t s;
                std::memcpy(&s, src + i * 2, 2);
                samples[i] = s / 32768.0f;
            }
        } else {
            ALOGE("loadKernelFromPath: unsupported WAV fmt=%u bits=%u", audioFmt, bitsPerSample);
            return;
        }

        const uint32_t kernelId = static_cast<uint32_t>(
            std::hash<std::string>{}(path) & 0xFFFFFFFFu);
        // File I/O complete. Acquire lock only around the DSP call.
        std::unique_lock lock(mMutex);
        auto result = mContext.viper().LoadConvolverKernel(
            samples.data(), frameCount, channels, kernelId);
        if (!result.has_value()) {
            ALOGE("loadKernelFromPath: LoadConvolverKernel failed for %s", path.c_str());
        } else {
            ALOGD("loadKernelFromPath: loaded %s id=0x%08x", path.c_str(), *result);
        }
    }

    // --- close / cleanup helpers ---

    void cleanupQueuesLocked() {
        // Destroy the EventFlag that was created in open().
        if (mEventFlag) {
            android::hardware::EventFlag::deleteEventFlag(&mEventFlag);
            mEventFlag = nullptr;
        }
        mWorkerBuf.clear();
        mWorkerBuf.shrink_to_fit();
        mInputMQ.reset();
        mOutputMQ.reset();
        mStatusMQ.reset();
    }

    void closeInternal() {
        std::unique_lock lock(mMutex);
        if (mState == State::INIT) return;
        stopWorkerLocked(lock);
        cleanupQueuesLocked();
        mState = State::INIT;
    }

    // --- members ---
    std::mutex   mMutex;
    State        mState      { State::INIT };
    uint32_t     mSampleRate { 48000 };
    size_t       mFrameCount { 256 };

    std::unique_ptr<DataMQ>   mInputMQ;
    std::unique_ptr<DataMQ>   mOutputMQ;
    std::unique_ptr<StatusMQ> mStatusMQ;

    // Persistent EventFlag: created once in open(), deleted in cleanupQueuesLocked().
    android::hardware::EventFlag* mEventFlag { nullptr };

    // Pre-allocated RT processing buffer; capacity set in open(), never reallocated.
    std::vector<float> mWorkerBuf;

    // jthread: auto-joins on destruction; stop_token replaces mWorkerExit atomic.
    std::jthread mWorkerThread;

    ViperContext mContext;

    // SHM channel: maps shm_params.bin + shm_bulk.bin for parameter polling.
    // Opened in open(), lives for the lifetime of the ViPEREffect instance.
    ShmChannel mShm;
};

// Static descriptor
const Descriptor ViPEREffect::kDescriptor = {
    .common = {
        .id = {
            .type  = kTypeUuid,
            .uuid  = kImplUuid,
            .proxy = std::nullopt,
        },
        .flags = {
            .type           = Flags::Type::INSERT,
            .insert         = Flags::Insert::FIRST,
            .volume         = Flags::Volume::NONE,
            .hwAcceleratorMode = Flags::HardwareAccelerator::NONE,
            .offloadIndication   = false,
            .deviceIndication    = false,
            .audioModeIndication = false,
            .audioSourceIndication = false,
        },
        .name        = std::string(kEffectName),
        .implementor = std::string(kImplementorName),
    }
};

// ---------------------------------------------------------------------------
// C entry points resolved by EffectFactory via dlsym
// ---------------------------------------------------------------------------
extern "C" {

[[gnu::visibility("default")]] binder_exception_t createEffect(
    const AudioUuid* in_impl_uuid,
    std::shared_ptr<aidl::android::hardware::audio::effect::IEffect>* instanceSpp
) {
    if (!in_impl_uuid || *in_impl_uuid != kImplUuid) {
        ALOGE("createEffect: uuid not supported");
        return EX_ILLEGAL_ARGUMENT;
    }
    if (!instanceSpp) return EX_ILLEGAL_ARGUMENT;
    *instanceSpp = ndk::SharedRefBase::make<ViPEREffect>();
    ALOGD("createEffect: instance %p created", instanceSpp->get());
    return EX_NONE;
}

[[gnu::visibility("default")]] binder_exception_t queryEffect(
    const AudioUuid* in_impl_uuid,
    Descriptor* _aidl_return
) {
    if (!in_impl_uuid || *in_impl_uuid != kImplUuid) {
        ALOGE("queryEffect: uuid not supported");
        return EX_ILLEGAL_ARGUMENT;
    }
    *_aidl_return = ViPEREffect::kDescriptor;
    return EX_NONE;
}

[[gnu::visibility("default")]] binder_exception_t destroyEffect(
    const std::shared_ptr<aidl::android::hardware::audio::effect::IEffect>& /*instanceSp*/
) {
    // SharedRefBase handles lifetime; nothing to do
    return EX_NONE;
}

} // extern "C"
