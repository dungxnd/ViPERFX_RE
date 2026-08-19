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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

// NDK binder
#include <android/binder_status.h>

// AIDL-generated NDK stubs (from aidl-gen/include/)
#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>
#include <fmq/AidlMessageQueue.h>

#include "ViperContext.h"
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
//   type : 7261726f-6d75-7369-6364-28e2fd3ac39e  (vendor/extension)
//   impl : 90380da3-8536-4744-a6a3-5731970e640f  (ViPER4Android)
// ---------------------------------------------------------------------------
// NOTE: AudioUuid::node is std::vector<uint8_t> in the AIDL NDK backend, which
// performs heap allocation even in constexpr evaluation contexts. A global
// constexpr/inline-constexpr object cannot retain a heap allocation past
// compile-time evaluation, so these must stay `const` (not `constexpr`).
const AudioUuid kTypeUuid = {
    static_cast<int32_t>(0x7261726f),
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
        (void)mContext.HandleCommand(EFFECT_CMD_INIT,       sizeof(int32_t), nullptr, &replySize, &reply);
        (void)mContext.HandleCommand(EFFECT_CMD_SET_CONFIG,  sizeof(cfg),    &cfg,    &replySize, &reply);

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
        int32_t  reply     = 0;
        switch (cmd) {
            case CommandId::START:
                if (mState == State::IDLE || mState == State::DRAINING) {
                    (void)mContext.HandleCommand(EFFECT_CMD_ENABLE, 0, nullptr, &replySize, &reply);
                    startWorkerLocked();
                    mState = State::PROCESSING;
                }
                break;
            case CommandId::STOP:
                if (mState == State::PROCESSING) {
                    stopWorkerLocked(lock);
                    (void)mContext.HandleCommand(EFFECT_CMD_DISABLE, 0, nullptr, &replySize, &reply);
                    mState = State::IDLE;
                }
                break;
            case CommandId::RESET:
                stopWorkerLocked(lock);
                (void)mContext.HandleCommand(EFFECT_CMD_RESET, 0, nullptr, &replySize, &reply);
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
        // p is const effect_param_t*; HandleCommand takes const void* for cmd_data — no cast needed.
        (void)mContext.HandleCommand(EFFECT_CMD_SET_PARAM,
                                     static_cast<uint32_t>(cmdSize),
                                     p,
                                     &replySize, &reply);
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
        std::vector<uint8_t> outBytes(kReplyBuf);
        // idBytes.data() is const uint8_t*; HandleCommand takes const void* for cmd_data.
        const auto* cmd = static_cast<const effect_param_t *>(
            static_cast<const void *>(idBytes.data()));
        auto* reply = static_cast<effect_param_t *>(static_cast<void *>(outBytes.data()));
        uint32_t replySize = static_cast<uint32_t>(kReplyBuf);
        {
            std::unique_lock lock(mMutex);
            (void)mContext.HandleCommand(EFFECT_CMD_GET_PARAM,
                                   static_cast<uint32_t>(idBytes.size()),
                                   cmd, &replySize, reply);
        }
        outBytes.resize(replySize);

        DefaultExtension outExt;
        outExt.bytes = std::move(outBytes);
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

        // Signal cooperative stop; wake the worker if blocked on EventFlag.
        mWorkerThread.request_stop();
        if (mEventFlag) {
            // Unconditionally set any bit so the EventFlag::wait() returns.
            static_cast<std::atomic<uint32_t>*>(
                    static_cast<void *>(mInputMQ->getEventFlagWord()))->fetch_or(1u, std::memory_order_release);
        }

        // Release mutex before joining: the worker may need the mutex to finish.
        lock.unlock();
        mWorkerThread.join();
        lock.lock();
    }

    void workerLoop(std::stop_token stopToken) {
        ALOGD("worker: started");
        constexpr size_t kChannels = 2;

        while (!stopToken.stop_requested()) {
            // mEventFlag was created once in open(); no system call here.
            if (mEventFlag) {
                uint32_t bits = 0;
                mEventFlag->wait(/*bitmask=*/0xFFFFFFFF, &bits,
                                 /*timeoutNs=*/100'000'000ULL, /*retry=*/true);
            }

            if (stopToken.stop_requested()) break;

            const size_t avail = mInputMQ->availableToRead();
            if (avail == 0) continue;

            // mWorkerBuf was reserve()d in open() to mFrameCount*4; resize never
            // reallocates as long as avail stays within that capacity.
            mWorkerBuf.resize(avail);
            if (!mInputMQ->read(mWorkerBuf.data(), avail)) continue;

            const size_t frames = avail / kChannels;
            audio_buffer_t inBuf  = { .frame_count = frames, .f32 = mWorkerBuf.data() };
            audio_buffer_t outBuf = { .frame_count = frames, .f32 = mWorkerBuf.data() };
            (void)mContext.Process(&inBuf, &outBuf);

            // Write processed samples to output MQ
            mOutputMQ->write(mWorkerBuf.data(), avail);

            // Signal status
            IEffect::Status st = { STATUS_OK, static_cast<int32_t>(avail),
                                              static_cast<int32_t>(avail) };
            mStatusMQ->write(&st, 1);
        }
        ALOGD("worker: stopped");
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
