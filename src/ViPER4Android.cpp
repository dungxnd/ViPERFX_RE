#include "ViperContext.h"
#include "essential.h"
#include "log.h"
#include "viper/constants.h"
#include <memory>
#include <new>

namespace {

struct ViperHandle {
    const effect_interface_s *interface{}; // Always keep as first member
    std::unique_ptr<ViperContext> context;
};

constexpr effect_descriptor_t kViperDescriptor = {
    .type = *EFFECT_UUID_NULL,
    .uuid = {0x90380da3, 0x8536, 0x4744, 0xa6a3, {0x57, 0x31, 0x97, 0x0e, 0x64, 0x0f}},
    .api_version = EFFECT_CONTROL_API_VERSION,
    .flags = EFFECT_FLAG_OUTPUT_DIRECT | EFFECT_FLAG_INPUT_DIRECT
             | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_TYPE_INSERT,
    .cpu_load = 8, // In 0.1 MIPS units as estimated on an ARM9E core (ARMv5TE) with 0 WS
    .memory_usage = 1, // In KB and includes only dynamically allocated memory
    .name = VIPER_NAME,
    .implementor = VIPER_AUTHORS
};

[[nodiscard]] bool IsMatchingUuid(const effect_uuid_t *uuid) {
    return uuid != nullptr && *uuid == kViperDescriptor.uuid;
}

int32_t ViperInterfaceProcess(
    effect_handle_t self, audio_buffer_t *in_buffer, audio_buffer_t *out_buffer
) {
    const auto viper_handle = reinterpret_cast<ViperHandle *>(self);
    if (viper_handle == nullptr) return -EINVAL;

    return viper_handle->context->Process(in_buffer, out_buffer);
}

int32_t ViperInterfaceCommand(
    effect_handle_t self,
    const uint32_t cmd_code,
    const uint32_t cmd_size,
    const void *cmd_data,
    uint32_t *reply_size,
    void *reply_data
) {
    const auto viper_handle = reinterpret_cast<ViperHandle *>(self);
    if (viper_handle == nullptr) return -EINVAL;

    return viper_handle->context->HandleCommand(
        cmd_code, cmd_size, cmd_data, reply_size, reply_data
    );
}

int32_t ViperInterfaceGetDescriptor(effect_handle_t self, effect_descriptor_t *descriptor) {
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
) {
    if (uuid == nullptr || handle == nullptr) return -EINVAL;
    if (!IsMatchingUuid(uuid)) {
        VIPER_LOGE(
            "ViperLibraryCreate: uuid mismatch (session=%d, io=%d, "
            "requested=%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x)",
            session_id,
            io_id,
            uuid->time_low,
            uuid->time_mid,
            uuid->time_hi_and_version,
            uuid->clock_seq,
            uuid->node[0],
            uuid->node[1],
            uuid->node[2],
            uuid->node[3],
            uuid->node[4],
            uuid->node[5]
        );
        return -ENOENT;
    }

    // v4a_re builds with -fno-exceptions: use nothrow allocation instead of try/catch.
    std::unique_ptr<ViperHandle> viper_handle{new (std::nothrow) ViperHandle()};
    if (viper_handle == nullptr) return -ENOMEM;

    viper_handle->interface = &kViperInterface;
    viper_handle->context.reset(new (std::nothrow) ViperContext());
    if (viper_handle->context == nullptr) return -ENOMEM;

    VIPER_LOGI(
        "ViperLibraryCreate: session_id=%d, io_id=%d, context=%p",
        session_id,
        io_id,
        viper_handle->context.get()
    );
    *handle = reinterpret_cast<effect_handle_t>(viper_handle.release());
    return 0;
}

int32_t ViperLibraryRelease(effect_handle_t handle) {
    const std::unique_ptr<ViperHandle> owned{reinterpret_cast<ViperHandle *>(handle)};
    if (owned == nullptr) return -EINVAL;

    VIPER_LOGI("ViperLibraryRelease: context=%p", owned->context.get());
    return 0;
}

int32_t ViperLibraryGetDescriptor(const effect_uuid_t *uuid, effect_descriptor_t *descriptor) {
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
