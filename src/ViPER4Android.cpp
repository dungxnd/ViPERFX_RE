#include "ViperContext.h"
#include "essential.h"
#include "log.h"
#include "viper/constants.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <new>
#include <type_traits>
#include <cstring>

namespace {

// Forward-declare the interface table so ViperHandle can reference it.
extern const effect_interface_s kViperInterface;

struct ViperHandle {
    // ABI requirement: interface pointer MUST be first member at offset 0 so
    // that effect_handle_t (a void*) can be safely reinterpret_cast<>d both ways.
    const effect_interface_s *interface = &kViperInterface;

    // Embed ViperContext directly — eliminates the second heap allocation and
    // the extra pointer indirection on every audio-processing call.
    ViperContext context{};
};

// Verify the C-ABI layout promise at compile time.
// Note: ViperContext contains std::atomic / std::vector / std::string members,
// so ViperHandle is NOT standard-layout; we only assert what actually matters
// for the ABI: that the interface pointer sits at byte offset 0.
static_assert(offsetof(ViperHandle, interface) == 0,
              "interface must be the first member of ViperHandle (C ABI requirement)");

constexpr effect_descriptor_t kViperDescriptor = {
    .type = *EFFECT_UUID_NULL,
    .uuid = {0x90380da3, 0x8536, 0x4744, 0xa6a3, {0x57, 0x31, 0x97, 0x0e, 0x64, 0x0f}},
    .api_version = EFFECT_CONTROL_API_VERSION,
    .flags = EFFECT_FLAG_OUTPUT_DIRECT | EFFECT_FLAG_INPUT_DIRECT
             | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_TYPE_INSERT,
    .cpu_load = 8,      // 0.1 MIPS units on ARM9E/ARMv5TE at 0 wait states
    .memory_usage = 1,  // KB, dynamic allocations only
    .name = VIPER_NAME,
    .implementor = VIPER_AUTHORS
};

[[nodiscard]] constexpr bool IsMatchingUuid(const effect_uuid_t *uuid) noexcept {
    return uuid != nullptr && *uuid == kViperDescriptor.uuid;
}

// Format a UUID into canonical 8-4-4-4-12 hex string for log messages.
[[nodiscard]] std::string FormatUuid(const effect_uuid_t &u) {
    return std::format(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        u.time_low, u.time_mid, u.time_hi_and_version, u.clock_seq,
        u.node[0], u.node[1], u.node[2], u.node[3], u.node[4], u.node[5]
    );
}

int32_t ViperInterfaceProcess(
    effect_handle_t self, audio_buffer_t *in_buffer, audio_buffer_t *out_buffer
) noexcept {
    // effect_handle_t is struct effect_interface_s**; the actual object is ViperHandle.
    // Round-trip through void* is the well-defined way to recover the original pointer.
    const auto viper_handle = static_cast<ViperHandle *>(static_cast<void *>(self));
    if (viper_handle == nullptr) return -EINVAL;

    return viper_handle->context.Process(in_buffer, out_buffer);
}

int32_t ViperInterfaceCommand(
    effect_handle_t self,
    const uint32_t cmd_code,
    const uint32_t cmd_size,
    const void *cmd_data,
    uint32_t *reply_size,
    void *reply_data
) noexcept {
    const auto viper_handle = static_cast<ViperHandle *>(static_cast<void *>(self));
    if (viper_handle == nullptr) return -EINVAL;

    // Cast void* → std::byte* at the boundary; HandleCommand uses typed bytes internally.
    return viper_handle->context.HandleCommand(
        cmd_code, cmd_size,
        static_cast<const std::byte *>(cmd_data),
        reply_size,
        static_cast<std::byte *>(reply_data)
    );
}

int32_t ViperInterfaceGetDescriptor(effect_handle_t /*self*/, effect_descriptor_t *descriptor) noexcept {
    if (descriptor == nullptr) return -EINVAL;
    *descriptor = kViperDescriptor;
    return 0;
}

constexpr effect_interface_s kViperInterface = {
    .process = ViperInterfaceProcess,
    .command = ViperInterfaceCommand,
    .get_descriptor = ViperInterfaceGetDescriptor
};

int32_t ViperLibraryCreate(
    const effect_uuid_t *uuid, int32_t session_id, int32_t io_id, effect_handle_t *handle
) noexcept {
    if (uuid == nullptr || handle == nullptr) return -EINVAL;

    if (!IsMatchingUuid(uuid)) {
        VIPER_LOGE(
            "ViperLibraryCreate: uuid mismatch (session=%d, io=%d, requested=%s)",
            session_id, io_id, FormatUuid(*uuid).c_str()
        );
        return -ENOENT;
    }

    // Single allocation: ViperHandle embeds ViperContext (no second heap alloc).
    // v4a_re builds with -fno-exceptions: use nothrow new instead of try/catch.
    std::unique_ptr<ViperHandle> viper_handle{new (std::nothrow) ViperHandle()};
    if (!viper_handle) return -ENOMEM;

    VIPER_LOGI(
        "ViperLibraryCreate: session_id=%d, io_id=%d, context=%p",
        session_id, io_id, static_cast<void *>(&viper_handle->context)
    );

    *handle = static_cast<effect_handle_t>(static_cast<void *>(viper_handle.release()));
    return 0;
}

int32_t ViperLibraryRelease(effect_handle_t handle) noexcept {
    const std::unique_ptr<ViperHandle> owned{static_cast<ViperHandle *>(static_cast<void *>(handle))};
    if (!owned) return -EINVAL;

    VIPER_LOGI("ViperLibraryRelease: context=%p", static_cast<void *>(&owned->context));
    return 0;
}

int32_t ViperLibraryGetDescriptor(const effect_uuid_t *uuid, effect_descriptor_t *descriptor) noexcept {
    if (uuid == nullptr || descriptor == nullptr) return -EINVAL;
    if (!IsMatchingUuid(uuid)) {
        VIPER_LOGE("ViperLibraryGetDescriptor: uuid mismatch");
        return -ENOENT;
    }

    *descriptor = kViperDescriptor;
    return 0;
}

} // namespace

extern "C" {
[[gnu::visibility("default")]] audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    .tag = AUDIO_EFFECT_LIBRARY_TAG,
    .version = EFFECT_LIBRARY_API_VERSION,
    .name = VIPER_NAME,
    .implementor = VIPER_AUTHORS,
    .create_effect = ViperLibraryCreate,
    .release_effect = ViperLibraryRelease,
    .get_descriptor = ViperLibraryGetDescriptor,
};
} // extern "C"
