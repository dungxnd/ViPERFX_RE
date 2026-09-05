# ViPERFX_RE

A reverse-engineered, modernized port of ViPER4Android. The DSP has been re-implemented from a decompilation of the original `libv4a_fx.so`, with audio processed in float32, dead code removed, and dependencies refreshed.

## Module Architecture

ViPERFX_RE ships as a **single unified Magisk / KernelSU / APatch module** built around the high-performance `ViPERDSP` engine. The installer (`customize.sh`) **auto-detects the device's audio HAL type at flash time** and installs the correct driver variant (`libv4a_aidl.so` or `libv4a_re.so`) automatically — you do not need to choose or flash separate packages.

```
┌─────────────────────────────────────────────────────────────┐
│                    ViPER4Android RE App                     │
└──────────────┬───────────────────────────────▲──────────────┘
               │                               │
       [AIDL / Android 13+]          [Non-AIDL / Android 5–14]
   Memory-Mapped SHM IPC Channel       Classic effect_param_t
    (/data/local/tmp/v4a/)                 via audioserver
               │                               │
               ▼                               ▼
       ┌───────────────┐               ┌───────────────┐
       │libv4a_aidl.so │               │ libv4a_re.so  │
       └───────┬───────┘               └───────┬───────┘
               │                               │
               └───────────────┬───────────────┘
                               ▼
               ┌───────────────────────────────┐
               │          ViPERDSP             │
               │   C++23/26 Engine (Planar)    │
               │ • 23 DSP audio effects        │
               │ • Pure float32 / NEON vector  │
               │ • DoubleBufferedState atomics │
               └───────────────────────────────┘
```

### How Auto-Detection Works

During module installation, `customize.sh` executes an **AOSP-aligned 7-tier symmetric scoring engine** to evaluate whether the host device runs a modern AIDL audio HAL or a legacy HIDL/classic audio HAL stack:

| Tier | Evaluation Target | Description | Score Weight |
| :--- | :--- | :--- | :--- |
| **Tier 0** | **Manual Overrides & Boundaries** | Forces mode via `/data/local/tmp/v4a_force_aidl` or `/data/local/tmp/v4a_force_legacy`. Android $\le 12$ (API $\le 32$) lacks AIDL audio HAL support and is strictly locked to Legacy. | Immediate / Override |
| **Tier 1** | **Definitive Runtime Services** | Checks `ServiceManager` for `android.hardware.audio.effect.IFactory/default` and `android.hardware.audio.core.IModule/default`, or probes `lshal` for AIDL vs HIDL (`IEffectsFactory` / `IDevicesFactory`) services. | $\pm 15$ to $20\text{ pts}$ |
| **Tier 2** | **VINTF Manifests** | Scans `/vendor/etc/vintf/`, `/odm/`, `/apex/`, and OEM product manifests for declared `format="aidl"` vs `format="hidl"` audio HAL entries. | $\pm 10$ to $15\text{ pts}$ |
| **Tier 3** | **Active HAL Daemons & Properties** | Probes active init services (`vendor.audio-hal-aidl`, `secaudiohalaidl`, `oplus`, `vivo`, `mediatek`) and running process trees. | $\pm 8$ to $10\text{ pts}$ |
| **Tier 4** | **Hardware Binaries & Libraries** | Inspects `/vendor/bin/hw/` and APEX modules for AIDL HAL binaries, or detects `libeffectproxy.so` (exclusive to HIDL). | $\pm 6\text{ pts}$ |
| **Tier 5** | **Effect Config Signatures** | Detects pure `audio_effects_config.xml` (AIDL) vs legacy `audio_effects.conf`. | $\pm 4$ to $6\text{ pts}$ |
| **Tier 6** | **OS Version Baseline Prior** | Android 15+ (API 35+) removed HIDL audio HAL support (+12 AIDL); Android 14 (API 34) defaults to AIDL (+4 AIDL). | Baseline |

### Driver Variants & Communication Protocols

#### 1. AIDL Variant (`libv4a_aidl.so`)
* **Platform:** Designed for Android 13+ (mandatory on Android 15+).
* **Audio Frame Path:** Audio frames and effect states travel across the standard **AOSP Fast Message Queue (Data MQ / Status MQ)** inside the vendor audio HAL process.
* **Control & Telemetry Channel:** Control parameters, impulse responses, and telemetry between the ViPER4Android companion app and the driver travel over a **lock-free, memory-mapped Shared Memory (SHM) interface** in `/data/local/tmp/v4a/`:
  - `shm_params.bin` (4096 bytes): Double-buffered slot exchange (magic `0x534D3456`, format version 6, update counter, active slot index, `ViPERParams` struct) ensuring zero audio glitching or locking on the real-time audio thread.
  - `shm_bulk.bin` (4096 bytes): Fast streaming sub-channel for DDC filter coefficients and Convolver impulse response file paths.
  - `shm_status.bin` (256 bytes): Live driver telemetry written directly by `libv4a_aidl.so` (enabled state, configured state, 64-bit processed frame counter, active sample rate, version code) read by the app to display real-time status.

#### 2. Non-AIDL / Legacy Variant (`libv4a_re.so`)
* **Platform:** For Android 5.0 through Android 14 devices utilizing classic HALs.
* **Audio & Control Path:** Runs inside `audioserver`. Parameters are passed via the standard Android `effect_param_t` command interface (`EFFECT_CMD_SET_PARAM` / `EFFECT_CMD_GET_PARAM`). No SHM files are used.

### Boot Sequence & System Integration

The module performs low-level integration at multiple stages of the Android boot process:

```
[Flashing: customize.sh]
  ├── Detects HAL type (AIDL vs Legacy) via 7-tier scoring
  ├── Installs 32-bit & 64-bit soundfx libraries
  ├── Discovers & patches all audio_effects*.xml/conf across OEM partitions
  └── Prepares partition symlinks for KernelSU / APatch

[Early Boot: post-fs-data.sh]
  ├── Pre-allocates /data/local/tmp/v4a/ SHM files with 0666/0777 permissions
  ├── Sets SELinux labels to u:object_r:shell_data_file:s0
  ├── Resets inhibition properties (ro.audio.ignore_effects=false)
  └── Injects live SELinux policies (sepolicy.rule / magiskpolicy / ksu_policy)

[Late Boot: service.sh]
  ├── Waits for sys.boot_completed=1
  ├── Enters init PID 1 mount namespace (nsenter -t 1 -m)
  ├── Shadow bind-mounts soundfx libraries and patched audio_effects configs
  ├── Restarts audio HAL daemons (audioserver, secaudiohalaidl, vendor.audio-hal-aidl)
  └── Spawns background guardian loop to keep effect properties enabled
```

## Installation

1. Download **the module zip** from the [Releases page](https://github.com/dungxnd/ViPERFX_RE/releases) and the [ViPER4Android app](https://github.com/dungxnd/ViPER4Android). There is a single zip — the installer picks AIDL or Legacy automatically.
2. Flash the module in Magisk / KernelSU / APatch.
3. Install the app.
4. Reboot. Open the app and verify effects are applied (use any of the diagnostic commands below to confirm).

> [!NOTE]
> - **KernelSU / APatch:** The installer automatically creates partition symlinks (`system/vendor -> vendor`, etc.). If your kernel/ROM environment requires mounting files directly into `/vendor` or `/system`, ensure overlay mounting is enabled or use a metamodule like `meta-overlayfs`.
> - **Audio Modification Library (AML):** AML is **incompatible** with the AIDL module variant because AML alters or strips AIDL effect registration definitions in `audio_effects_config.xml`. Disable AML when running on Android 14+ AIDL setups.

## Troubleshooting & Diagnostics

All installation, mount, and daemon restart logs are written to:
```bash
adb shell su -c 'cat /data/adb/viper_install.log'
```

### 1. Identify Which Mode Was Installed
Check the installation decision recorded by the installer:
```bash
adb shell su -c 'cat /data/adb/modules/ViPER4Android-RE/aidl_mode.txt'
```
* Returns `aidl` or `legacy`.

---

### 2. Diagnostics for AIDL Variant

#### Check Driver Telemetry (SHM Status)
The AIDL driver continuously writes heartbeat frames and active sample rate to `shm_status.bin`. Inspect the raw status header:
```bash
adb shell su -c 'od -tx4 -N 32 /data/local/tmp/v4a/shm_status.bin'
```
* Offset `0x00`: Magic `0x534D3456` (`V4MS`)
* Offset `0x08`: Monotonic status sequence counter (increments on every driver tick)
* Offset `0x14`: `isEnabled` (`1` = enabled, `0` = disabled)
* Offset `0x1c`: 64-bit processed frame counter (accumulates while audio plays)

#### Check AIDL Audio HAL Logs
```bash
adb logcat -d -s 'ViPER4Android:*' 'AHAL_EffectImpl:*'
```
Expected output:
```
ViPER4Android: Welcome to ViPER FX
ViPER4Android: ViPER (AIDL) context created, sample_rate=48000
ViPER4Android: AudioEffect created successfully for session 0
ViPER4Android: Global effect created (aidlType=true)
```

#### Check Config Registration
Verify that `audio_effects_config.xml` includes `libv4a_aidl.so`:
```bash
adb shell su -c 'grep -E "v4a_aidl|90380da3" /vendor/etc/audio_effects_config.xml'
```

#### Verify Audio HAL Process
Find the vendor audio HAL process and inspect its open files:
```bash
adb shell "ps -A | grep -iE 'audio.*aidl|hardware.audio'"
```

---

### 3. Diagnostics for Non-AIDL (Legacy) Variant

#### Check Driver Initialization
```bash
adb logcat -d -s 'ViPER4Android:*'
```
Expected output:
```
ViPER4Android: Welcome to ViPER FX
ViPER4Android: ViperContext created
```

#### Inspect AudioFlinger Effect Chain
```bash
adb shell su -c 'dumpsys media.audio_flinger | grep -E -A5 -B7 "90380da3"'
```
Expected output:
```
Descriptor:
- UUID: 90380da3-8536-4744-a6a3-5731970e640f
- TYPE: ec7178ec-e5e1-4432-a3f4-4657e6795210
- name: ViPERDSP
```

#### Verify Config Registration
```bash
adb shell su -c 'grep -r "libv4a_re.so" /vendor/etc/audio_effects*.xml /system/etc/audio_effects*.xml 2>/dev/null'
```

---

### 4. SELinux Audit Denials (Both Variants)
If the driver fails to load or cannot access shared memory, check for SELinux denials:
```bash
adb logcat -d | grep -E 'avc.*denied.*(v4a|soundfx|audio)'
```

---

## Building from Source

### Prerequisites
- Linux or macOS environment (or WSL on Windows).
- Android NDK (r27+ recommended). Set `ANDROID_NDK_HOME` or `ANDROID_NDK_ROOT`.
- CMake 3.30+, Make, Ninja, and Python 3.

### Build Targets

```bash
# 1. Run host unit tests (MSVC / Clang / GCC with AddressSanitizer & UndefinedSanitizer)
make test

# 2. Fetch bleeding-edge AOSP Clang for C++26 ViPERDSP static library compilation
make aosp-clang-fetch

# 3. Compile the static ViPERDSP core for arm64-v8a and armeabi-v7a
make dsp-arm64-v8a
make dsp-armeabi-v7a

# 4. Build Non-AIDL driver libraries (libv4a_re.so)
make libs

# 5. Generate AIDL C++ stubs from AOSP hardware interfaces
make aidl-gen

# 6. Build AIDL driver libraries (libv4a_aidl.so)
make aidl-libs

# 7. Package flashable Magisk/KernelSU/APatch module zip
make zip
```
The packaged module will be generated under `out/ViPER4Android-RE-<version>.zip`.

---

## Disclaimers

- **Hardware Diversity:** While tested extensively on Pixel, Samsung, Xiaomi, and MediaTek devices across Android 10 through Android 16, OEM audio implementations vary widely. Always maintain a full device backup before flashing root modules.
- **Audio Fidelity:** The DSP algorithms are modernized cleanroom implementations based on reverse-engineering of `libv4a_fx.so`. Subtle nuances in ballistics and filtering may differ from legacy 32-bit builds.
- **Licensing:** Reverse-engineered for compatibility and preservation. Not for commercial use.

---

## Credits

- **Zhuhang & ViPER520** — Creators of the original ViPER4Android.
- **Martmists, Iscle, llsl** — Reverse-engineering of the original DSP routines.
- **pittvandewitt, MrWhite214, Zackptg5** — Android audio effects development and module packaging.
- **DungxND** — C++23/26 modernization, planar SIMD architecture, and unified AIDL HAL engine.
