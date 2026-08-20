cmake_minimum_required(VERSION 3.30)
project("ViPER4Android")

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VERSION_CODE "00000000" CACHE STRING "Build version code")
set(VERSION_NAME "0.0.0" CACHE STRING "Build version name")

add_compile_definitions(VERSION_CODE=${VERSION_CODE})
add_compile_definitions(VERSION_NAME="${VERSION_NAME}")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_options(
    -O3 -DNDEBUG
    -ffunction-sections -fdata-sections
    -fno-exceptions -fno-rtti
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -fvisibility=hidden -fvisibility-inlines-hidden)

add_subdirectory(ViPERDSP)

# ----------------------------------------------------------------------------
# Non-AIDL (legacy) module
# ----------------------------------------------------------------------------
add_library(v4a_re SHARED
    src/ViPER4Android.cpp
    src/ViperContext.cpp
)

target_include_directories(v4a_re PRIVATE
    src
    src/include
)

target_link_libraries(v4a_re ViPERDSP log)

target_compile_options(v4a_re PRIVATE
    -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)

target_link_options(v4a_re PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
    -Wl,--exclude-libs,ALL
    -fvisibility=hidden)

# ----------------------------------------------------------------------------
# AIDL module (Android 13+ / API 33+)
#
# Requires AIDL-generated C++ stubs in ${AIDL_GEN_DIR} (default: aidl-gen/)
# and the NDK binder_ndk_cpp optional headers.
# Both are provided by the CI "Generate AIDL stubs" step in build.yml; for
# local builds run: make aidl-gen   (see Makefile).
# ----------------------------------------------------------------------------
option(BUILD_AIDL "Build the AIDL audio effect module (requires aidl-gen/)" OFF)

if(BUILD_AIDL)
    set(AIDL_GEN_DIR "${CMAKE_SOURCE_DIR}/aidl-gen" CACHE PATH
        "Directory containing AIDL-generated NDK C++ sources")

    # NDK r28+ removed binder_interface_utils.h and friends from the NDK sysroot.
    # The aidl-gen step copies them from frameworks/native into aidl-gen/include/android/.
    # ViPER4Android_AIDL.cpp implements BnEffect directly — no AOSP EffectImpl dependency —
    # so only aidl-gen/include (AIDL stubs + libfmq + binder headers) is required.

    # Collect every generated .cpp under aidl-gen/src/
    file(GLOB_RECURSE AIDL_SRCS "${AIDL_GEN_DIR}/src/*.cpp")

    add_library(v4a_aidl SHARED
        src/ViPER4Android_AIDL.cpp
        src/ViperContext.cpp
        ${AIDL_SRCS}
    )

    target_include_directories(v4a_aidl PRIVATE
        src
        src/include
        ${AIDL_GEN_DIR}/include
        # Shim sub-directories written by the aidl-gen step (cutils/, utils/, android-base/, fmq/)
        # are all under ${AIDL_GEN_DIR}/include, which is already listed above.
        # The entries below make include paths like <fmq/EventFlag.h> resolve to the shims
        # rather than to any sysroot headers that may shadow them.
        ${AIDL_GEN_DIR}/include/fmq
    )

    # AIDL module needs exceptions and RTTI (BnEffect / shared_ptr).
    # Remove the global -fno-exceptions/-fno-rtti.
    target_compile_options(v4a_aidl PRIVATE
        -fexceptions -frtti
        -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)

    target_link_libraries(v4a_aidl ViPERDSP log binder_ndk android)

    target_link_options(v4a_aidl PRIVATE
        -Wl,--gc-sections
        -Wl,--strip-all
        -Wl,--exclude-libs,ALL
        -fvisibility=hidden)

    # Minimum API for the AIDL effect HAL is 33
    set_target_properties(v4a_aidl PROPERTIES
        ANDROID_PLATFORM android-33)
endif()
