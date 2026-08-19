# ViPERFX_RE
# Builds libv4a_re.so (non-AIDL) and libv4a_aidl.so (AIDL) for Android

# NDK: ANDROID_NDK_HOME > ANDROID_NDK_ROOT > ANDROID_HOME/ndk/<latest>
ANDROID_NDK_HOME ?= $(or $(ANDROID_NDK_ROOT),$(wildcard $(ANDROID_HOME)/ndk/$(shell ls $(ANDROID_HOME)/ndk 2>/dev/null | sort -V | tail -1)))
ifeq ($(ANDROID_NDK_HOME),)
  $(warning ANDROID_NDK_HOME not set. Set it to your NDK installation path.)
endif

NDK_TOOLCHAIN  := $(ANDROID_NDK_HOME)/build/cmake/android.toolchain.cmake
MIN_SDK        := 21
MIN_SDK_AIDL   := 33
ALL_ABIS       := armeabi-v7a arm64-v8a
BUILD_TYPE     := Release
AIDL_GEN_DIR   := aidl-gen

# Version defaults read from module.prop (overridable via CLI)
VERSION_NAME   ?= $(shell grep '^version=' module/module.prop | cut -d= -f2)
VERSION_CODE   ?= $(shell grep '^versionCode=' module/module.prop | cut -d= -f2)

# ABI selection: pass ABI= to build a subset (comma or space separated)
COMMA := ,
ifdef ABI
  ABIS := $(subst $(COMMA), ,$(ABI))
else
  ABIS := $(ALL_ABIS)
endif

BUILD_DIR      := build
OUT_DIR        := out
MODULE_DIR     := module
MODULE_OUT     := $(OUT_DIR)/magisk_module
MODULE_ZIP     := $(OUT_DIR)/ViPER4Android-RE-$(VERSION_NAME).zip

ADB_DEVICE     := $(shell adb devices 2>/dev/null | awk 'NR==2 && $$2=="device"{print $$1}')
ADB            := adb$(if $(ADB_DEVICE), -s $(ADB_DEVICE),)

.PHONY: all clean libs aidl-gen aidl-libs module zip deploy test $(ALL_ABIS)

all: libs

# ── Host unit tests (no NDK required; uses system g++ and GTest via FetchContent) ──
test:
	@echo "Building and running host unit tests..."
	@mkdir -p $(BUILD_DIR)/tests
	cmake -B $(BUILD_DIR)/tests \
		-G Ninja \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
		tests
	cmake --build $(BUILD_DIR)/tests -- -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	ctest --test-dir $(BUILD_DIR)/tests --output-on-failure --parallel $$(nproc 2>/dev/null || echo 4)


# Build selected ABIs (non-AIDL)
libs: $(ABIS)

# Per-ABI build targets (non-AIDL)
$(ABIS):
	@echo "Building for ABI: $@"
	@mkdir -p $(BUILD_DIR)/$@
	cmake -B $(BUILD_DIR)/$@ \
		-DCMAKE_TOOLCHAIN_FILE=$(NDK_TOOLCHAIN) \
		-DANDROID_ABI=$@ \
		-DANDROID_PLATFORM=android-$(MIN_SDK) \
		-DANDROID_ARM_NEON=TRUE \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DVERSION_CODE=$(VERSION_CODE) \
		-DVERSION_NAME=$(VERSION_NAME) \
		.
	cmake --build $(BUILD_DIR)/$@ -- -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	@mkdir -p $(OUT_DIR)
	cp $(BUILD_DIR)/$@/libv4a_re.so $(OUT_DIR)/libv4a_re_$@.so

# Generate AIDL C++ stubs locally (requires Android SDK build-tools on PATH or ANDROID_HOME)
# CI runs this step automatically; run once locally before 'make aidl-libs'.
aidl-gen:
	@echo "Generating AIDL stubs into $(AIDL_GEN_DIR)/ ..."
	$(eval AIDL_BIN := $(shell ls $(ANDROID_HOME)/build-tools/*/aidl 2>/dev/null | sort -V | tail -1))
	@test -n "$(AIDL_BIN)" || (echo "ERROR: aidl not found. Install Android SDK build-tools."; exit 1)
	@echo "Using aidl: $(AIDL_BIN)"
	@rm -rf aosp-interfaces-tmp aosp-system-interfaces-tmp aosp-frameworks-native-tmp
	git clone --filter=blob:none --no-checkout --depth=1 \
		https://android.googlesource.com/platform/hardware/interfaces \
		aosp-interfaces-tmp
	cd aosp-interfaces-tmp && \
		git sparse-checkout init --cone && \
		git sparse-checkout set \
			audio/aidl/android/hardware/audio/effect \
			audio/aidl/android/hardware/audio/common \
			audio/aidl/aidl_api/android.hardware.audio.effect/3 \
			common/aidl/android/hardware/common \
			common/fmq/aidl/android/hardware/common/fmq && \
		git checkout
	git clone --filter=blob:none --no-checkout --depth=1 \
		https://android.googlesource.com/platform/system/hardware/interfaces \
		aosp-system-interfaces-tmp
	cd aosp-system-interfaces-tmp && \
		git sparse-checkout init --cone && \
		git sparse-checkout set \
			media/aidl/android/media/audio/common \
			media/aidl/android/media/audio/eraser && \
		git checkout
	git clone --filter=blob:none --no-checkout --depth=1 \
		https://android.googlesource.com/platform/frameworks/native \
		aosp-frameworks-native-tmp
	cd aosp-frameworks-native-tmp && \
		git sparse-checkout init --cone && \
		git sparse-checkout set libs/binder/ndk/include_cpp/android && \
		git checkout
	@mkdir -p $(AIDL_GEN_DIR)/src $(AIDL_GEN_DIR)/include/android \
		$(AIDL_GEN_DIR)/include/fmq $(AIDL_GEN_DIR)/include/cutils \
		$(AIDL_GEN_DIR)/include/utils $(AIDL_GEN_DIR)/include/android-base
	cp aosp-frameworks-native-tmp/libs/binder/ndk/include_cpp/android/*.h $(AIDL_GEN_DIR)/include/android/
	# Copy committed NDK-only shim headers (replaces the platform libfmq headers)
	cp -r $(CURDIR)/aidl-shims/include/. $(AIDL_GEN_DIR)/include/
	$(AIDL_BIN) --lang=ndk --structured --stability=vintf \
		-I aosp-interfaces-tmp/audio/aidl \
		-I aosp-interfaces-tmp/common/aidl \
		-I aosp-interfaces-tmp/common/fmq/aidl \
		-I aosp-system-interfaces-tmp/media/aidl \
		-o $(AIDL_GEN_DIR)/src \
		--header_out $(AIDL_GEN_DIR)/include \
		aosp-interfaces-tmp/common/aidl/android/hardware/common/*.aidl
	$(AIDL_BIN) --lang=ndk --structured --stability=vintf \
		-I aosp-interfaces-tmp/audio/aidl \
		-I aosp-interfaces-tmp/common/aidl \
		-I aosp-interfaces-tmp/common/fmq/aidl \
		-I aosp-system-interfaces-tmp/media/aidl \
		-o $(AIDL_GEN_DIR)/src \
		--header_out $(AIDL_GEN_DIR)/include \
		aosp-interfaces-tmp/common/fmq/aidl/android/hardware/common/fmq/*.aidl
	$(AIDL_BIN) --lang=ndk --structured --stability=vintf \
		-I aosp-interfaces-tmp/audio/aidl \
		-I aosp-interfaces-tmp/common/aidl \
		-I aosp-interfaces-tmp/common/fmq/aidl \
		-I aosp-system-interfaces-tmp/media/aidl \
		-o $(AIDL_GEN_DIR)/src \
		--header_out $(AIDL_GEN_DIR)/include \
		aosp-interfaces-tmp/audio/aidl/android/hardware/audio/common/*.aidl
	$(AIDL_BIN) --lang=ndk --structured --stability=vintf \
		-I aosp-interfaces-tmp/audio/aidl \
		-I aosp-interfaces-tmp/common/aidl \
		-I aosp-interfaces-tmp/common/fmq/aidl \
		-I aosp-system-interfaces-tmp/media/aidl \
		-o $(AIDL_GEN_DIR)/src \
		--header_out $(AIDL_GEN_DIR)/include \
		aosp-system-interfaces-tmp/media/aidl/android/media/audio/common/*.aidl \
		aosp-system-interfaces-tmp/media/aidl/android/media/audio/eraser/*.aidl
	$(AIDL_BIN) --lang=ndk --structured --stability=vintf \
		--min_sdk_version=31 \
		-I aosp-interfaces-tmp/audio/aidl \
		-I aosp-interfaces-tmp/common/aidl \
		-I aosp-interfaces-tmp/common/fmq/aidl \
		-I aosp-system-interfaces-tmp/media/aidl \
		-o $(AIDL_GEN_DIR)/src \
		--header_out $(AIDL_GEN_DIR)/include \
		aosp-interfaces-tmp/audio/aidl/android/hardware/audio/effect/*.aidl
	@rm -rf aosp-interfaces-tmp aosp-system-interfaces-tmp aosp-frameworks-native-tmp
	@echo "AIDL stubs generated in $(AIDL_GEN_DIR)/src and $(AIDL_GEN_DIR)/include"

# Build AIDL .so for all selected ABIs (requires aidl-gen/ to exist)
aidl-libs: $(addprefix aidl-,$(ABIS))

aidl-%:
	@echo "Building AIDL module for ABI: $*"
	@test -d $(AIDL_GEN_DIR) || (echo "ERROR: run 'make aidl-gen' first"; exit 1)
	@mkdir -p $(BUILD_DIR)/aidl/$*
	cmake -B $(BUILD_DIR)/aidl/$* \
		-DCMAKE_TOOLCHAIN_FILE=$(NDK_TOOLCHAIN) \
		-DANDROID_ABI=$* \
		-DANDROID_PLATFORM=android-$(MIN_SDK_AIDL) \
		-DANDROID_ARM_NEON=TRUE \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DVERSION_CODE=$(VERSION_CODE) \
		-DVERSION_NAME=$(VERSION_NAME) \
		-DBUILD_AIDL=ON \
		-DAIDL_GEN_DIR=$(CURDIR)/$(AIDL_GEN_DIR) \
		.
	cmake --build $(BUILD_DIR)/aidl/$* -- -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	@mkdir -p $(OUT_DIR)
	cp $(BUILD_DIR)/aidl/$*/libv4a_aidl.so $(OUT_DIR)/libv4a_aidl_$*.so

# Prepare Magisk/KSU/APatch module directory (auto-detects AIDL vs legacy at flash time).
# Ships both libv4a_re and libv4a_aidl .so files; install.sh picks the right one
# by checking for a running AIDL audio HAL process (or API level / static FS).
# Pass SKIP_LIBS=1 when .so files are already present in out/.
module: $(if $(SKIP_LIBS),,libs aidl-libs)
	@echo "Preparing Magisk/KSU/APatch module..."
	@rm -rf $(MODULE_OUT)
	@mkdir -p $(MODULE_OUT)/common/files
	@cp -r $(MODULE_DIR)/META-INF $(MODULE_OUT)/
	@cp $(MODULE_DIR)/module.prop $(MODULE_OUT)/module.prop
	@cp $(MODULE_DIR)/customize.sh $(MODULE_OUT)/
	@cp $(MODULE_DIR)/post-fs-data.sh $(MODULE_OUT)/post-fs-data.sh
	@cp $(MODULE_DIR)/post-mount.sh $(MODULE_OUT)/post-mount.sh 2>/dev/null || true
	@cp $(MODULE_DIR)/uninstall.sh $(MODULE_OUT)/
	@cp $(MODULE_DIR)/LICENSE $(MODULE_OUT)/
	@cp $(MODULE_DIR)/sepolicy.rule $(MODULE_OUT)/sepolicy.rule 2>/dev/null || true
	@cp -r $(MODULE_DIR)/common/* $(MODULE_OUT)/common/
	@sed -i.bak \
		-e 's/^version=.*/version=$(VERSION_NAME)/' \
		-e 's/^versionCode=.*/versionCode=$(VERSION_CODE)/' \
		$(MODULE_OUT)/module.prop && rm -f $(MODULE_OUT)/module.prop.bak
	@for abi in $(ABIS); do \
		cp $(OUT_DIR)/libv4a_re_$$abi.so   $(MODULE_OUT)/common/files/ 2>/dev/null || true; \
		cp $(OUT_DIR)/libv4a_aidl_$$abi.so $(MODULE_OUT)/common/files/ 2>/dev/null || true; \
	done

# Create flashable zip
zip: module
	@echo "Creating Magisk module zip..."
	@mkdir -p $(OUT_DIR)
	@rm -f $(MODULE_ZIP)
	cd $(MODULE_OUT) && zip -r9 $(CURDIR)/$(MODULE_ZIP) . -x '*.DS_Store'
	@echo "Module: $(MODULE_ZIP)"

# Build single ABI aliases
arm64: arm64-v8a
arm32: armeabi-v7a

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR) aosp-interfaces-tmp aosp-system-interfaces-tmp aosp-frameworks-native-tmp

format:
	@echo "Formatting code with clang-format..."
	@find src/ -name '*.[ch]' -o -name '*.cpp' | xargs clang-format -i

help:
	@echo "ViPERFX_RE Build System"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - Android NDK (set ANDROID_NDK_HOME)"
	@echo "  - CMake"
	@echo "  - Android SDK build-tools (for AIDL targets, set ANDROID_HOME)"
	@echo ""
	@echo "Targets:"
	@echo "  make libs          Build libv4a_re.so for all ABIs (default)"
	@echo "  make arm64-v8a     Build for arm64 only"
	@echo "  make armeabi-v7a   Build for arm32 only"
	@echo "  make aidl-gen      Generate AIDL C++ stubs (needs SDK build-tools)"
	@echo "  make aidl-libs     Build libv4a_aidl.so for all ABIs"
	@echo "  make module        Prepare Magisk module directory (auto-detects AIDL)"
	@echo "  make zip           Build + package Magisk zip"
	@echo "  make test          Build and run host unit tests (no NDK required)"
	@echo "  make clean         Remove build artifacts"
	@echo ""
	@echo "Options:"
	@echo "  ANDROID_NDK_HOME=<path>  NDK installation path"
	@echo "  ANDROID_HOME=<path>      Android SDK path (for aidl-gen)"
	@echo "  MIN_SDK=<num>            Minimum SDK for non-AIDL (default: 21)"
	@echo "  BUILD_TYPE=<type>        CMake build type (default: Release)"
	@echo "  VERSION_NAME=<str>       Version name (default: from module.prop)"
	@echo "  VERSION_CODE=<num>       Version code (default: from module.prop)"
	@echo "  ABI=<abi[,abi]>          Build specific ABI(s) (default: all)"
