cmake_minimum_required(VERSION 3.30)
project(ViPERFX_RE_Tests C CXX)

if (MSVC)
    set(CMAKE_CXX_STANDARD 23)
else ()
    set(CMAKE_CXX_STANDARD 26)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Required for CTest to recognise this as a testable project.
enable_testing()

# ---------------------------------------------------------------------------
# Host build: suppress Android-only flags / definitions
# ---------------------------------------------------------------------------
# Remove any -fno-exceptions / -fno-rtti inherited from parent (GTest needs them)
if (MSVC)
    add_compile_options(/W3)
else ()
    add_compile_options(-Wall -Wno-unused-parameter)
endif ()

# VERSION_CODE and VERSION_NAME are defined by the top-level Android build.
# Provide harmless defaults for the host test build so ParameterRouter.h compiles.
add_compile_definitions(VERSION_CODE=0)
add_compile_definitions(VERSION_NAME="test")

# ---------------------------------------------------------------------------
# GTest via FetchContent (no system package required)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ---------------------------------------------------------------------------
# ViPERDSP — build the DSP library for the host
# ---------------------------------------------------------------------------
# The submodule uses add_subdirectory already in the top-level CMakeLists.txt,
# but the tests CMakeLists is invoked standalone (or via the test target),
# so we add the submodule here with the host guard.
set(VIPERDSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ViPERDSP")

# Collect sources (mirrors ViPERDSP/CMakeLists.txt).
# GLOB_RECURSE with the top-level viper/ directory captures effects/, utils/, ViPER.cpp, etc.
file(GLOB_RECURSE VIPERDSP_SRCS
    "${VIPERDSP_DIR}/viper/*.cpp"
    "${VIPERDSP_DIR}/viper/*.c"
)

add_library(ViPERDSP_host STATIC ${VIPERDSP_SRCS})
target_include_directories(ViPERDSP_host PUBLIC
    "${VIPERDSP_DIR}/include"
    "${VIPERDSP_DIR}"
)
if (MSVC)
    target_compile_options(ViPERDSP_host PRIVATE /O2)
else ()
    target_compile_options(ViPERDSP_host PRIVATE
        -O2
        -Wno-sign-compare
        -Wno-parentheses
    )
endif ()
# Host: no Android log, no ARM-specific asm guards already handled in log.h
# _USE_MATH_DEFINES: required on MSVC/MinGW to expose M_PI from <cmath>.
target_compile_definitions(ViPERDSP_host PUBLIC HOST_BUILD=1 _USE_MATH_DEFINES)

# ---------------------------------------------------------------------------
# ViperContext object library (shared by integration tests)
# Compile ViperContext.cpp for the host (no HAL, essential.h is self-contained)
# ---------------------------------------------------------------------------
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_library(viper_context_host OBJECT
    "${SRC_DIR}/ViperContext.cpp"
)
target_include_directories(viper_context_host PUBLIC
    "${SRC_DIR}"
    "${SRC_DIR}/include"
    "${VIPERDSP_DIR}/include"
    "${VIPERDSP_DIR}"
)
target_compile_definitions(viper_context_host PUBLIC HOST_BUILD=1)
target_link_libraries(viper_context_host PUBLIC ViPERDSP_host)

# ---------------------------------------------------------------------------
# Helper macro — add a test executable and register it with CTest
# ---------------------------------------------------------------------------
macro(viper_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        "${SRC_DIR}"
        "${SRC_DIR}/include"
        "${VIPERDSP_DIR}/include"
        "${VIPERDSP_DIR}"
    )
    target_compile_definitions(${name} PRIVATE HOST_BUILD=1)
    target_link_libraries(${name} PRIVATE
        viper_context_host
        ViPERDSP_host
        GTest::gtest_main
    )
    # PRE_TEST: discovery runs when ctest executes, not at build time.
    # This is more robust when sanitisers are active (POST_BUILD can silently
    # fail to run the binary, leaving CTest with zero registered tests).
    gtest_discover_tests(${name} TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
endmacro()

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------
viper_test(test_format_converter)
viper_test(test_parameter_router)
viper_test(test_stream_supervisor)
viper_test(test_viper_context)
viper_test(test_dsp_effects)
# test_dsp_effects includes effect headers directly (e.g. "effects/SoftwareLimiter.h"),
# so it needs the ViPERDSP/viper/ directory on its include path.
target_include_directories(test_dsp_effects PRIVATE "${VIPERDSP_DIR}/viper")
