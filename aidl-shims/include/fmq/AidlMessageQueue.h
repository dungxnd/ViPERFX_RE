// Shim: AidlMessageQueue — NDK-only replacement for the platform libfmq header.
// Uses only mmap + ASharedMemory (API 26+) + Linux futex. No libutils/libcutils linkage.
#pragma once
#include <aidl/android/hardware/common/fmq/MQDescriptor.h>
#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/common/fmq/UnsynchronizedWrite.h>
#include <fmq/AidlMQDescriptorShim.h>
#include <fmq/EventFlag.h>
#include <cutils/ashmem.h>
#include <cutils/native_handle.h>
#include <android/binder_parcel.h>
#include <sys/mman.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <type_traits>
#include <vector>

using android::hardware::kSynchronizedReadWrite;
using android::hardware::kUnsynchronizedWrite;
using android::hardware::MQFlavor;

namespace android {

using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::common::fmq::UnsynchronizedWrite;
using android::details::AidlMQDescriptorShim;

// ── Flavour-to-value mapping ────────────────────────────────────────────────
template <typename T> struct FlavorTypeToValue;
template <> struct FlavorTypeToValue<SynchronizedReadWrite> {
    static constexpr MQFlavor value = hardware::kSynchronizedReadWrite;
};
template <> struct FlavorTypeToValue<UnsynchronizedWrite> {
    static constexpr MQFlavor value = hardware::kUnsynchronizedWrite;
};

// Grantor layout (must match the convention used in AOSP libfmq)
enum FmqGrantorPos { READPTRPOS = 0, WRITEPTRPOS, DATAPTRPOS, EVFLAGWORDPOS };

// ── AidlMessageQueue ─────────────────────────────────────────────────────────
template <typename T, typename U>
struct AidlMessageQueue {
    using Descriptor = AidlMQDescriptorShim<T, FlavorTypeToValue<U>::value>;

    /// Construct from a descriptor received over binder (consumer side).
    explicit AidlMessageQueue(const MQDescriptor<T, U> &desc, bool resetPointers = true);
    /// Allocate a new shared-memory queue (producer side).
    explicit AidlMessageQueue(size_t numElements, bool configureEventFlagWord = false);
    ~AidlMessageQueue();

    bool   isValid()        const { return mRing != nullptr; }
    size_t getQuantumSize() const { return sizeof(T); }
    size_t getQuantumCount()const { return mCapacity; }
    size_t availableToWrite()const;
    size_t availableToRead() const;

    bool write(const T *data, size_t count);
    bool read (T *data,       size_t count);

    /// Return a descriptor that can be sent to another process.
    MQDescriptor<T, U> dupeDesc();

    /// Pointer to the event-flag word (used with EventFlag::createEventFlag).
    std::atomic<uint32_t> *getEventFlagWord() const { return mEvFlagWord; }

private:
    static constexpr size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
    static constexpr size_t kPageSize = 4096;

    size_t mCapacity   = 0;
    uint8_t *mRing     = nullptr;   // points into the shared mapping
    std::atomic<uint64_t> *mReadPtr  = nullptr;
    std::atomic<uint64_t> *mWritePtr = nullptr;
    std::atomic<uint32_t> *mEvFlagWord = nullptr;
    int    mFd         = -1;
    size_t mMappedSize = 0;

    AidlMessageQueue(const AidlMessageQueue &) = delete;
    AidlMessageQueue &operator=(const AidlMessageQueue &) = delete;
};

// ── Producer constructor ──────────────────────────────────────────────────────
template <typename T, typename U>
AidlMessageQueue<T, U>::AidlMessageQueue(size_t numElements, bool configureEventFlagWord)
    : mCapacity(numElements) {
    const size_t dataSize = numElements * sizeof(T);
    const size_t ctrlSize = alignUp(2 * sizeof(uint64_t), kPageSize);
    const size_t evSize   = configureEventFlagWord
                                ? alignUp(sizeof(uint32_t), kPageSize) : 0;
    mMappedSize = ctrlSize + alignUp(dataSize, kPageSize) + evSize;

    mFd = ashmem_create_region("AidlMQ", mMappedSize);
    if (mFd < 0) return;

    auto *base = static_cast<uint8_t *>(
        mmap(nullptr, mMappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, 0));
    if (base == MAP_FAILED) { close(mFd); mFd = -1; return; }

    mReadPtr  = reinterpret_cast<std::atomic<uint64_t> *>(base);
    mWritePtr = reinterpret_cast<std::atomic<uint64_t> *>(base + sizeof(uint64_t));
    mReadPtr->store(0);
    mWritePtr->store(0);
    mRing = base + ctrlSize;

    if (configureEventFlagWord)
        mEvFlagWord = reinterpret_cast<std::atomic<uint32_t> *>(
            base + ctrlSize + alignUp(dataSize, kPageSize));
}

// ── Consumer constructor ──────────────────────────────────────────────────────
template <typename T, typename U>
AidlMessageQueue<T, U>::AidlMessageQueue(const MQDescriptor<T, U> &desc, bool resetPointers) {
    const auto &dg = desc.grantors;
    if (dg.size() < 3 || desc.handle.fds.empty()) return;

    mFd = dup(desc.handle.fds[0].get());
    if (mFd < 0) return;

    // Compute the total span we need to map.
    size_t maxEnd = 0;
    for (const auto &g : dg) {
        size_t end = static_cast<size_t>(g.offset) + static_cast<size_t>(g.extent);
        if (end > maxEnd) maxEnd = end;
    }
    mMappedSize = alignUp(maxEnd, kPageSize);
    mCapacity   = static_cast<size_t>(dg[DATAPTRPOS].extent) / sizeof(T);

    auto *base = static_cast<uint8_t *>(
        mmap(nullptr, mMappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, 0));
    if (base == MAP_FAILED) { close(mFd); mFd = -1; return; }

    mReadPtr  = reinterpret_cast<std::atomic<uint64_t> *>(base + dg[READPTRPOS].offset);
    mWritePtr = reinterpret_cast<std::atomic<uint64_t> *>(base + dg[WRITEPTRPOS].offset);
    if (resetPointers) { mReadPtr->store(0); mWritePtr->store(0); }
    mRing = base + dg[DATAPTRPOS].offset;

    if (dg.size() > EVFLAGWORDPOS)
        mEvFlagWord = reinterpret_cast<std::atomic<uint32_t> *>(
            base + dg[EVFLAGWORDPOS].offset);
}

// ── Destructor ────────────────────────────────────────────────────────────────
template <typename T, typename U>
AidlMessageQueue<T, U>::~AidlMessageQueue() {
    if (mReadPtr)  // base of the mapping is mReadPtr
        munmap(static_cast<void *>(mReadPtr), mMappedSize);
    if (mFd >= 0) close(mFd);
}

// ── Read / write ──────────────────────────────────────────────────────────────
template <typename T, typename U>
size_t AidlMessageQueue<T, U>::availableToRead() const {
    const uint64_t w = mWritePtr->load(std::memory_order_acquire);
    const uint64_t r = mReadPtr ->load(std::memory_order_acquire);
    return static_cast<size_t>(w >= r ? w - r : mCapacity - (r - w));
}

template <typename T, typename U>
size_t AidlMessageQueue<T, U>::availableToWrite() const {
    return mCapacity - availableToRead();
}

template <typename T, typename U>
bool AidlMessageQueue<T, U>::write(const T *data, size_t count) {
    if (count > availableToWrite()) return false;
    const size_t w = static_cast<size_t>(
        mWritePtr->load(std::memory_order_relaxed) % mCapacity);
    const size_t chunk1 = std::min(count, mCapacity - w);
    memcpy(mRing + w * sizeof(T), data,            chunk1 * sizeof(T));
    memcpy(mRing,                 data + chunk1, (count - chunk1) * sizeof(T));
    mWritePtr->fetch_add(count, std::memory_order_release);
    return true;
}

template <typename T, typename U>
bool AidlMessageQueue<T, U>::read(T *data, size_t count) {
    if (count > availableToRead()) return false;
    const size_t r = static_cast<size_t>(
        mReadPtr->load(std::memory_order_relaxed) % mCapacity);
    const size_t chunk1 = std::min(count, mCapacity - r);
    memcpy(data,            mRing + r * sizeof(T), chunk1 * sizeof(T));
    memcpy(data + chunk1,   mRing,                 (count - chunk1) * sizeof(T));
    mReadPtr->fetch_add(count, std::memory_order_release);
    return true;
}

// ── dupeDesc ─────────────────────────────────────────────────────────────────
template <typename T, typename U>
MQDescriptor<T, U> AidlMessageQueue<T, U>::dupeDesc() {
    MQDescriptor<T, U> desc;
    desc.quantum = static_cast<int32_t>(sizeof(T));
    desc.flags   = static_cast<int32_t>(FlavorTypeToValue<U>::value);

    const size_t ctrlSize = alignUp(2 * sizeof(uint64_t), kPageSize);
    const size_t dataSize = alignUp(mCapacity * sizeof(T), kPageSize);

    using GD = aidl::android::hardware::common::fmq::GrantorDescriptor;
    desc.grantors = {
        GD{.fdIndex = 0, .offset = 0,
           .extent = static_cast<int64_t>(sizeof(uint64_t))},
        GD{.fdIndex = 0, .offset = static_cast<int32_t>(sizeof(uint64_t)),
           .extent = static_cast<int64_t>(sizeof(uint64_t))},
        GD{.fdIndex = 0, .offset = static_cast<int32_t>(ctrlSize),
           .extent = static_cast<int64_t>(mCapacity * sizeof(T))},
    };
    if (mEvFlagWord)
        desc.grantors.push_back(
            GD{.fdIndex = 0, .offset = static_cast<int32_t>(ctrlSize + dataSize),
               .extent = static_cast<int64_t>(sizeof(uint32_t))});

    desc.handle.fds.push_back(ndk::ScopedFileDescriptor(dup(mFd)));
    return desc;
}

} // namespace android
