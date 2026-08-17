// NDK-compatible shim for <cutils/native_handle.h>
// Provides the native_handle_t struct and helpers using only POSIX/libc
// (android/native_handle.h does NOT exist in the public NDK sysroot)
#pragma once

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NATIVE_HANDLE_MAX_FDS  1024
#define NATIVE_HANDLE_MAX_INTS 1024

#define NATIVE_HANDLE_DECLARE_STORAGE(name, maxFds, maxInts)                  \
    alignas(native_handle_t) char (name)[                                      \
        sizeof(native_handle_t) + sizeof(int) * ((maxFds) + (maxInts))]

typedef struct native_handle {
    int version;  /* sizeof(native_handle_t) */
    int numFds;   /* number of file-descriptors at &data[0] */
    int numInts;  /* number of ints at &data[numFds] */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
    int data[0];  /* numFds + numInts ints */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} native_handle_t;

typedef const native_handle_t* buffer_handle_t;

static inline native_handle_t* native_handle_create(int numFds, int numInts) {
    if (numFds < 0 || numInts < 0 ||
        numFds > NATIVE_HANDLE_MAX_FDS || numInts > NATIVE_HANDLE_MAX_INTS)
        return NULL;
    size_t sz = sizeof(native_handle_t) + sizeof(int) * ((size_t)numFds + (size_t)numInts);
    native_handle_t* h = (native_handle_t*)malloc(sz);
    if (!h) return NULL;
    h->version = sizeof(native_handle_t);
    h->numFds  = numFds;
    h->numInts = numInts;
    return h;
}

static inline native_handle_t* native_handle_init(char* storage, int numFds, int numInts) {
    native_handle_t* h = (native_handle_t*)storage;
    h->version = sizeof(native_handle_t);
    h->numFds  = numFds;
    h->numInts = numInts;
    return h;
}

static inline int native_handle_close(const native_handle_t* h) {
    if (!h) return -1;
    for (int i = 0; i < h->numFds; ++i) close(h->data[i]);
    return 0;
}

static inline void native_handle_delete(native_handle_t* h) {
    free(h);
}

#ifdef __cplusplus
}
#endif
