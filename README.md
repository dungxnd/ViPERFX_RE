# ViPERFX_RE

A reverse-engineered, modernized port of ViPER4Android. The DSP has been re-implemented from a decompilation of the original `libv4a_fx.so`, with audio processed in float32, dead code removed, and dependencies refreshed.

## Module Architecture

ViPERFX_RE ships as **two separate Magisk modules**, both built around the same DSP engine (`ViPERDSP`) but integrated through different Android audio HAL interfaces. They are not interchangeable: each module is designed for a specific generation of Android’s audio framework. Installing the wrong one may either do nothing or cause boot issues.

### Non-AIDL (Legacy) Module

The non-AIDL module uses the **classic Android audio effect plugin interface** (often informally referred to as the “HIDL-based” path due to its association with the older audio HAL stack).

- **Why it no longer works on Android 15+:** Google has migrated the audio effect framework from the legacy C-based plugin model to a stable AIDL HAL implementation. Devices launching with Android 15 no longer support loading `effect_handle_t` plugins, as the old framework path has been removed entirely. See [AIDL for HALs](https://source.android.com/docs/core/architecture/aidl/aidl-hals) for more details.

### AIDL Module

The AIDL module implements the **modern audio effect HAL** introduced in Android 13, which became the only officially supported audio effect path for devices launching with Android 15 and later.

### Which module should you install?

Run the following command:

```bash
adb shell ps -A | grep "audio.*aidl"
```

If your see any process related to audio HAL with "aidl" in the name, you need the AIDL module. If not, the non-AIDL module should work.

## Disclaimers

- **Tested hardware is narrow.** Non-AIDL is confirmed on Pixel 8 Pro / Android 14. AIDL is confirmed on Pixel 8 Pro / Android 16. Other devices may bootloop, may need vendor-specific shims (e.g. ShadoV's [PIXAML](https://github.com/ShadoV90/PIXAML) for AIDL on some Pixels), or may simply do nothing. Make a backup before flashing.
- **Audio fidelity is best-effort.** The DSP is decompiled from `libv4a_fx.so`. Subtle deviations from the original ViPER4Android are expected. If you spot one, please open a PR — the source is here precisely so it can be improved.
- **Not for commercial use.** This is a reverse-engineering project and may carry legal restrictions in your jurisdiction. Use at your own risk.

## Installation

1. Download the **module zip matching your device** (non-AIDL vs. AIDL — see [Which module should you install?](#which-module-should-you-install)) from the [Releases page](https://github.com/dungxnd/ViPERFX_RE/releases), and the [ViPER4Android app](https://github.com/dungxnd/ViPER4Android).
2. Flash the Magisk module. **Do not flash both modules.**
3. Install the app.
4. Reboot. Open the app and verify effects are applied (use any of the diagnostic commands below to confirm).

## Troubleshooting

> [!NOTE]
> Both modules mount the driver `.so` and the `audio_effects*.xml` config into `/vendor` (and `/system` where present). This is verified with **MagiskSU**. If you use **KernelSU** or **APatch**, you may need a metamodule that allows mounting files into `/vendor` and `/system`. And the AIDL module is confirmed not compatible with **AudioModificationLibrary**, so disable it if you want to use AIDL module.

> [!IMPORTANT]
> When opening an issue, capture the relevant logs *while reproducing the problem* and attach them. The commands below are listed in the order you should run them when troubleshooting. Run the section that matches your module — the **non-AIDL** steps use `dumpsys`/`logcat` only (no SHM files), while the **AIDL** steps additionally inspect the shared-memory files.

### Log Tags

Both drivers (and the shared DSP) log under a single tag, **`ViPER4Android`**. The `AHAL_*` tags come from the AOSP AIDL effect framework and appear only on the AIDL path.

| Tag                  | Module     | Level   |
| -------------------- | ---------- | ------- |
| `ViPER4Android`      | both       | D/I/E   |
| `AHAL_EffectImpl`    | AIDL only  | D/I/V/E |
| `AHAL_EffectContext` | AIDL only  | E       |
| `AHAL_EffectThread`  | AIDL only  | V       |

### Non-AIDL (Legacy)

The non-AIDL driver is `libv4a_re.so`. It receives parameters over the classic `effect_param_t` command interface and creates **no** shared-memory files.

#### 1. Check if the driver is loaded

```bash
adb logcat -d -s 'ViPER4Android:*' | grep -E 'Welcome|version|created'
```

Expected (the version line must match the module you flashed):

```bash
ViPER4Android: Welcome to ViPER FX
ViPER4Android: Current version is ...
ViPER4Android: ViperContext created
```

If missing, the audio framework never loaded the `.so`. Verify the config was patched (`audio_effects.xml` under `/vendor/etc` and/or `/system/etc`):

```bash
adb shell su -c 'grep -r v4a /vendor/etc /system/etc 2>/dev/null'
```

Expected:

```bash
/vendor/etc/audio_effects.xml:        <library name="v4a_re" path="libv4a_re.so"/>
/vendor/etc/audio_effects.xml:        <effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>
/system/etc/audio_effects.xml:        <library name="v4a_re" path="libv4a_re.so"/>
/system/etc/audio_effects.xml:        <effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>
```

Then verify the SELinux label on the library itself:

```bash
adb shell su -c 'ls -Z /vendor/lib*/soundfx/libv4a_re.so'
```

Expected:

```bash
u:object_r:vendor_file:s0 /vendor/lib/soundfx/libv4a_re.so
u:object_r:vendor_file:s0 /vendor/lib64/soundfx/libv4a_re.so
```

#### 2. Dump the audioserver effect list

```bash
adb shell su -c 'dumpsys media.audio_flinger | grep -E -A5 -B7 "90380da3"'
```

Expected:

```bash
1 effects for session 0
    In buffer                         Out buffer                           Active tracks:
    0xb40000718d612da0 -> 0x72579a1000   0x72579a1000 -> 0xb40000718d612da0   0
    Effect ID 11:
        Session State Registered Internal Enabled Suspended:
        00000   002   y          n        y       n
        Descriptor:
        - UUID: 90380da3-8536-4744-a6a3-5731970e640f
        - TYPE: ec7178ec-e5e1-4432-a3f4-4657e6795210
        - apiVersion: 00000000
        - flags: 00005010 (conn. mode: insert, insert pref: last, volume mgmt: none, input mode: direct, output mode: direct)
        - name: ViPERDSP
        - implementor: viper.WYF, Martmists, Iscle, llsl
```

#### 3. Check SELinux denials

```bash
adb logcat -d -s audit | grep v4a
# or, broader:
adb logcat -d | grep -E 'avc.*denied.*(v4a|soundfx)'
```

If you see `avc: denied` lines naming the audio process and the driver `.so`, the live policy injection from `post-fs-data.sh` did not stick. This is the single most common cause of "module installs cleanly but audio is unprocessed." The legacy effect runs inside `audioserver` (or the legacy audio HAL), not the AIDL service.

### AIDL

The AIDL driver is `libv4a_aidl.so`. It receives the full parameter state through memory-mapped **shared-memory** files under `/data/local/tmp/v4a/`.

#### 1. Check if the AIDL driver is loaded

```bash
adb logcat -d -s 'ViPER4Android:*' | grep -E 'Welcome|version|created'
```

Expected (the version line must match the module you flashed):

```bash
ViPER4Android: Welcome to ViPER FX
ViPER4Android: Current version is ...
ViPER4Android: ViPER (AIDL) context created, sample_rate=...
ViPER4Android: AudioEffect created successfully for session 0
ViPER4Android: Global effect created (aidlType=true)
```

If missing, the audio framework never loaded the `.so`. Verify the config was patched:

```bash
adb shell su -c 'grep v4a /vendor/etc/audio_effects_config.xml'
```

Expected:

```bash
<library name="v4a_aidl" path="libv4a_aidl.so"/>
<effect name="v4a_standard_aidl" library="v4a_aidl" uuid="90380da3-8536-4744-a6a3-5731970e640f" type="7261676f-6d75-7369-6364-28e2fd3ac39e"/>
```

Then verify the SELinux label on the library itself:

```bash
adb shell su -c 'ls -Z /vendor/lib*/soundfx/libv4a_aidl.so'
```

Expected:

```bash
u:object_r:vendor_file:s0 /vendor/lib/soundfx/libv4a_aidl.so
u:object_r:vendor_file:s0 /vendor/lib64/soundfx/libv4a_aidl.so
```

#### 2. Dump the audioserver effect list

```bash
adb shell su -c 'dumpsys media.audio_flinger | grep -E -A5 -B7 "90380da3"'
```

Expected:

```bash
1 effects for session 0
    In buffer                               Out buffer                                 Active tracks:
    0xb40000778e915020 -> 0xb40000778e959220   0xb40000778e959220 -> 0xb40000778e915020   0
    Effect ID 1035:
        Session State Registered Internal Enabled Suspended:
        00000   003   y          n        y       n
        Descriptor:
        - UUID: 90380da3-8536-4744-a6a3-5731970e640f
        - TYPE: 7261676f-6d75-7369-6364-28e2fd3ac39e
        - apiVersion: 00020000
        - flags: 00410208 (conn. mode: insert, insert pref: first, volume mgmt: none, device indication: requires updates, input mode: not set, output mode: not set, hardware acceleration: non-tunneled, offloadable)
        - name: ViPER4Android
        - implementor: ViPER520 / RE Team
```

#### 3. Check the shared-memory files

The AIDL driver receives its state through three memory-mapped files under `/data/local/tmp/v4a/`.

```bash
adb shell su -c 'ls -laZ /data/local/tmp/v4a/'
```

Expected (v2 layout — a single merged `shm_params.bin`, no more `shm_hp.bin`/`shm_spk.bin`):

```bash
drwxrwxrwx   root        root  ...          kernel          # staged convolver kernels
-rw-rw-rw- 1 root        root  ... 4096 ... shm_bulk.bin    # DDC + convolver bulk push
-rw-rw-rw- 1 audioserver audio ... 4096 ... shm_params.bin  # double-buffered effect params
-rw-rw-rw- 1 root        root  ...  256 ... shm_status.bin  # driver status + version
```

Inspect the SHM headers (magic `V4MS` = `5634 4d53`, format version `0600`):

```bash
adb shell su -c 'xxd -l 8 /data/local/tmp/v4a/shm_status.bin'
adb shell su -c 'xxd -l 8 /data/local/tmp/v4a/shm_params.bin'
adb shell su -c 'xxd -l 8 /data/local/tmp/v4a/shm_bulk.bin'
```

Expected (all three start with the same magic + version `0600`):

```bash
00000000: 5634 4d53 0600 0000                      V4MS....
00000000: 5634 4d53 0600 0000                      V4MS....
00000000: 5634 4d53 0600 0000                      V4MS....
```

If the magic is wrong, the files are truncated, or the version does not match the flashed module, the module install did not complete — reflash and reboot.

#### 4. Check SELinux denials

```bash
adb logcat -d -s audit | grep v4a
# or, broader:
adb logcat -d | grep -E 'avc.*denied.*(v4a|shm|soundfx|shell_data_file)'
```

If you see `avc: denied` lines naming the audio HAL process and the driver `.so` or the SHM files, the live policy injection from `post-fs-data.sh` did not stick. This is the single most common cause of "module installs cleanly but audio is unprocessed."

#### 5. Filter logcat by the audio HAL process

```bash
# Find the audio HAL process name (device-specific)
adb shell ps -A | grep audio
```

Expected (Pixel 8 Pro):

```bash
audioserver   ...  S android.hardware.audio.service-aidl.aoc
audioserver   ...  S audioserver
```

Then filter logcat by the HAL PID:

```bash
adb logcat --pid=$(adb shell pidof android.hardware.audio.service-aidl.aoc | tr -d '\r') -s 'ViPER4Android:*' 'AHAL_EffectImpl:*'
```

**The HAL process name is device-specific.**

## Building

Prerequisites: Android NDK, CMake, Make. Set `ANDROID_NDK_HOME` (or `ANDROID_NDK_ROOT`).

```bash
make libs     # build libv4a_re.so for arm64-v8a and armeabi-v7a
make module   # build + stage Magisk module directory
make zip      # build + package a flashable Magisk module zip
```

## Credits

- Zhuhang and ViPER520 — original ViPER4Android.
- Martmists, Iscle, llsl — reverse-engineering of the DSP.
