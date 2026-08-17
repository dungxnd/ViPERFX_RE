// Shim: minimal HIDL-flavour types required by AidlMQDescriptorShim
#pragma once
#include <cstdint>
#include <string>
namespace android {
namespace hardware {
enum MQFlavor : uint32_t {
    kSynchronizedReadWrite = 0x01,
    kUnsynchronizedWrite   = 0x02,
};
struct GrantorDescriptor { uint32_t flags; int32_t fdIndex; int32_t offset; int64_t extent; };
namespace details {
inline void logError(const std::string & /*msg*/) {}
} // namespace details
} // namespace hardware
} // namespace android
