# ViPERFX_RE

A reverse-engineered, modernized port of ViPER4Android. The DSP has been re-implemented from a decompilation of the original `libv4a_fx.so`, with audio processed in float32, dead code removed, and dependencies refreshed.

## Module Architecture

ViPERFX_RE ships as a **single unified Magisk/KSU/APatch module** built around the `ViPERDSP` engine. The installer (`install.sh`) **auto-detects the device's audio HAL type at flash time** and installs the correct variant automatically — you do not need to choose or flash two modules.

### How auto-detection works

The installer probes five signals in priority order:

1. **Legacy negative guard** — presence of `audio_effects.conf`, `audio_effects.xml`, or `libeffectproxy.so` suggests a legacy stack.
2. **VINTF override** — if a VINTF manifest in `/vendor/etc/vintf/`, `/vendor/manifest.xml`, `/odm/etc/vintf/`, or `/system/etc/vintf/` declares `android.hardware.audio.effect`, the device runs AIDL regardless of legacy XML files left for 32-bit compat.
3. **Static filesystem** — AIDL HAL binaries in `/vendor/bin/hw/` or `/apex/`, or the AOSP AIDL config filename `audio_effects_config.xml`.
4. **Runtime property** — `init.svc.vendor.audio-hal-aidl = running`.
5. **API tiebreaker** — API ≥ 35 defaults to AIDL only when no FS evidence was found (AOSP removed legacy HAL in Android 15, but many OEMs keep legacy stacks).

### Non-AIDL (Legacy) variant

Uses the **classic Android audio effect plugin interface** (often informally referred to as the "HIDL-based" path).

- **Why it no longer works on Android 15+:** Google migrated the audio effect framework to a stable AIDL HAL implementation. Devices launching with Android 15 no longer support loading `effect_handle_t` plugins. See [AIDL for HALs](https://source.android.com/docs/core/architecture/aidl/aidl-hals) for more details.

### AIDL variant

Implements the **modern audio effect HAL** introduced in Android 13, which became the only officially supported audio effect path for devices launching with Android 15 and later. Uses the standard AOSP AIDL Fast Message Queue (FMQ) for parameter passing — no custom shared-memory files are required.

## Disclaimers

- **Tested hardware is narrow.** Non-AIDL is confirmed on Pixel 8 Pro / Android 14. AIDL is confirmed on Pixel 8 Pro / Android 16. Other devices may bootloop, may need vendor-specific shims (e.g. ShadoV's [PIXAML](https://github.com/ShadoV90/PIXAML) for AIDL on some Pixels), or may simply do nothing. Make a backup before flashing.
- **Audio fidelity is best-effort.** The DSP is decompiled from `libv4a_fx.so`. Subtle deviations from the original ViPER4Android are expected. If you spot one, please open a PR — the source is here precisely so it can be improved.
- **Not for commercial use.** This is a reverse-engineering project and may carry legal restrictions in your jurisdiction. Use at your own risk.

## Installation

1. Download **the module zip** from the [Releases page](https://github.com/dungxnd/ViPERFX_RE/releases) and the [ViPER4Android app](https://github.com/dungxnd/ViPER4Android). There is a single zip — the installer picks AIDL or Legacy automatically.
2. Flash the module in Magisk / KernelSU / APatch.
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

The AIDL driver is `libv4a_aidl.so`. Parameters are exchanged via the standard **AOSP AIDL Fast Message Queue (FMQ)** — no custom shared-memory files are created or required.

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

#### 3. Check SELinux denials

```bash
adb logcat -d -s audit | grep v4a
# or, broader:
adb logcat -d | grep -E 'avc.*denied.*(v4a|soundfx)'
```

If you see `avc: denied` lines naming the audio HAL process and the driver `.so`, the live policy injection from `sepolicy.rule` did not stick. This is the single most common cause of "module installs cleanly but audio is unprocessed."

#### 4. Filter logcat by the audio HAL process

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
