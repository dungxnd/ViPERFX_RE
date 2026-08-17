// Shim: AidlMQDescriptorShim — NDK-only replacement for the platform libfmq header.
// Depends only on <cutils/native_handle.h> and <fmq/MQDescriptorBase.h> (both shims).
#pragma once
#include <aidl/android/hardware/common/fmq/MQDescriptor.h>
#include <cutils/native_handle.h>
#include <fmq/MQDescriptorBase.h>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace android {
namespace details {

using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::common::fmq::UnsynchronizedWrite;
using android::hardware::MQFlavor;

template <typename T, MQFlavor flavor>
struct AidlMQDescriptorShim {
    using FlavorType = typename std::conditional<
        flavor == hardware::kSynchronizedReadWrite,
        SynchronizedReadWrite, UnsynchronizedWrite>::type;

    // Construct from AIDL MQDescriptor (takes ownership of handle fds via dup)
    explicit AidlMQDescriptorShim(const MQDescriptor<T, FlavorType> &desc);
    // Construct from raw grantors + handle (takes ownership of handle)
    AidlMQDescriptorShim(const std::vector<hardware::GrantorDescriptor> &grantors,
                         native_handle_t *nHandle, size_t size);
    // Size-only constructor used internally
    AidlMQDescriptorShim(size_t bufferSize, native_handle_t *nHandle,
                         size_t messageSize, bool configureEventFlag = false);
    explicit AidlMQDescriptorShim(const AidlMQDescriptorShim &other)
        : AidlMQDescriptorShim(0, nullptr, 0) { *this = other; }
    AidlMQDescriptorShim &operator=(const AidlMQDescriptorShim &other);
    ~AidlMQDescriptorShim();

    size_t   getSize()    const;
    size_t   getQuantum() const;
    uint32_t getFlags()   const;
    bool     isHandleValid()  const { return mHandle != nullptr; }
    size_t   countGrantors()  const { return mGrantors.size(); }

    const std::vector<hardware::GrantorDescriptor> &grantors() const { return mGrantors; }
    const ::native_handle_t *handle() const { return mHandle; }
          ::native_handle_t *handle()       { return mHandle; }

    // Required by MessageQueueBase but unused in our shim — define to satisfy the ABI.
    static const size_t kOffsetOfGrantors;
    static const size_t kOffsetOfHandle;

private:
    std::vector<hardware::GrantorDescriptor> mGrantors;
    native_handle_t *mHandle = nullptr;
    uint32_t mQuantum = 0;
    uint32_t mFlags   = 0;
};

// ── Template implementations ──────────────────────────────────────────────────

template <typename T, MQFlavor flavor>
AidlMQDescriptorShim<T, flavor>::AidlMQDescriptorShim(
        const MQDescriptor<T, FlavorType> &desc)
    : mQuantum(static_cast<uint32_t>(desc.quantum)),
      mFlags  (static_cast<uint32_t>(desc.flags)) {
    if (desc.quantum < 0 || desc.flags < 0) return;
    mGrantors.resize(desc.grantors.size());
    for (size_t i = 0; i < desc.grantors.size(); ++i) {
        const auto &g = desc.grantors[i];
        if (g.offset < 0 || g.extent < 0 || g.fdIndex < 0) return;
        mGrantors[i] = {0u, g.fdIndex, g.offset, g.extent};
    }
    mHandle = native_handle_create(
        static_cast<int>(desc.handle.fds.size()),
        static_cast<int>(desc.handle.ints.size()));
    if (!mHandle) return;
    int idx = 0;
    for (const auto &fd  : desc.handle.fds)  mHandle->data[idx++] = dup(fd.get());
    for (const auto &val : desc.handle.ints) mHandle->data[idx++] = val;
}

template <typename T, MQFlavor flavor>
AidlMQDescriptorShim<T, flavor>::AidlMQDescriptorShim(
        const std::vector<hardware::GrantorDescriptor> &grantors,
        native_handle_t *nHandle, size_t size)
    : mGrantors(grantors),
      mHandle(nHandle),
      mQuantum(static_cast<uint32_t>(size)),
      mFlags(flavor) {}

template <typename T, MQFlavor flavor>
AidlMQDescriptorShim<T, flavor>::AidlMQDescriptorShim(
        size_t /*bufferSize*/, native_handle_t *nHandle,
        size_t messageSize, bool /*configureEventFlag*/)
    : mHandle(nHandle),
      mQuantum(static_cast<uint32_t>(messageSize)),
      mFlags(flavor) {}

template <typename T, MQFlavor flavor>
AidlMQDescriptorShim<T, flavor> &
AidlMQDescriptorShim<T, flavor>::operator=(const AidlMQDescriptorShim &o) {
    if (this == &o) return *this;
    mGrantors = o.mGrantors;
    if (mHandle) { native_handle_close(mHandle); native_handle_delete(mHandle); mHandle = nullptr; }
    mQuantum = o.mQuantum;
    mFlags   = o.mFlags;
    if (o.mHandle) {
        mHandle = native_handle_create(o.mHandle->numFds, o.mHandle->numInts);
        if (mHandle) {
            for (int i = 0; i < o.mHandle->numFds; ++i)
                mHandle->data[i] = dup(o.mHandle->data[i]);
            memcpy(&mHandle->data[o.mHandle->numFds],
                   &o.mHandle->data[o.mHandle->numFds],
                   static_cast<size_t>(o.mHandle->numInts) * sizeof(int));
        }
    }
    return *this;
}

template <typename T, MQFlavor flavor>
AidlMQDescriptorShim<T, flavor>::~AidlMQDescriptorShim() {
    if (mHandle) { native_handle_close(mHandle); native_handle_delete(mHandle); }
}

template <typename T, MQFlavor flavor>
size_t AidlMQDescriptorShim<T, flavor>::getSize() const {
    return mGrantors.empty() ? 0u : static_cast<size_t>(mGrantors[0].extent);
}

template <typename T, MQFlavor flavor>
size_t AidlMQDescriptorShim<T, flavor>::getQuantum() const { return mQuantum; }

template <typename T, MQFlavor flavor>
uint32_t AidlMQDescriptorShim<T, flavor>::getFlags() const { return mFlags; }

} // namespace details
} // namespace android
