// Shim: ashmem_create_region() via NDK ASharedMemory_create() (API 26+)
#pragma once
#include <sys/types.h>
#ifdef __ANDROID_API__
#  if __ANDROID_API__ >= 26
#    include <android/sharedmem.h>
static inline int ashmem_create_region(const char *name, size_t size) {
    return ASharedMemory_create(name, size);
}
#  else
#    include <linux/ashmem.h>
#    include <fcntl.h>
static inline int ashmem_create_region(const char *name, size_t size) {
    int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return fd;
    if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0) { close(fd); return -1; }
    return fd;
}
#  endif
#endif
static inline int ashmem_valid(int fd) { return fd >= 0; }
