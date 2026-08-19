# ── AIDL detection ────────────────────────────────────────────────────────────
# Detection priority (highest confidence first):
#
#   1. Negative guard: if legacy effect .so already exists on the device
#      (e.g. libeffectproxy.so or the legacy audio_effects.conf), this is a
#      strong OEM signal the device shipped with a legacy audio stack regardless
#      of API level. OEMs like OnePlus ship Android 15 with legacy HAL.
#
#   2. Static FS — AIDL-positive: actual AIDL HAL binaries or config present.
#      Works in recovery/offline mode. Most reliable cross-OEM positive signal.
#        a. AIDL HAL executable in /vendor/bin/hw/ or /apex/
#        b. VINTF manifest declares android.hardware.audio.effect (AIDL iface)
#        c. audio_effects_config.xml (AOSP AIDL standard filename, distinct
#           from legacy audio_effects.conf / audio_effects.xml)
#
#   3. Runtime property: init.svc.vendor.audio-hal-aidl = "running".
#      Most reliable when booted, but only canonical AOSP service names —
#      misses OEM-renamed daemons.
#
#   4. ps -A scan: catches OEM daemons whose binary name contains both
#      "audio" and "aidl" in either order.
#
#   5. API >= 35 last-resort tiebreaker ONLY when no FS evidence was found.
#      AOSP removed legacy HAL in Android 15, but many OEMs (OnePlus, Xiaomi,
#      Samsung) still ship legacy stacks on API 35 devices. Do NOT use this
#      as a primary signal.
ui_print "- Detecting audio HAL type..."
USE_AIDL=false
LEGACY_CONFIRMED=false

# ── Signal 1: Negative guard — legacy stack evidence ─────────────────────────
# Presence of legacy effects config / libeffectproxy.so is a soft legacy signal.
# It is NOT a hard block: Android 14/15/16 AIDL-only devices (Pixel 8+, Snapdragon
# 8 Gen 3 OEMs) keep audio_effects.xml for 32-bit vendor blob backward compat while
# running a pure AIDL audio HAL. VINTF manifest (Signal 2b) can override this guess.
if ls /vendor/etc/audio_effects.conf 1>/dev/null 2>&1 || \
   ls /vendor/etc/audio_effects.xml 1>/dev/null 2>&1 || \
   ls /vendor/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1 || \
   ls /system/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1; then
  LEGACY_CONFIRMED=true
fi

# ── Signal 1b: VINTF override ─────────────────────────────────────────────────
# If Signal 1 fired but the VINTF manifest (authoritative OEM SoC declaration)
# explicitly lists the AIDL audio effect interface, the legacy XML files are kept
# only for 32-bit compat — the device runs a true AIDL stack. Override the guess.
if $LEGACY_CONFIRMED; then
  if grep -rq "android.hardware.audio.effect" /vendor/etc/vintf/ 2>/dev/null || \
     grep -rq "android.hardware.audio.effect" /odm/etc/vintf/ 2>/dev/null; then
    ui_print "    ! VINTF declares AIDL effect iface — overriding legacy XML signal"
    LEGACY_CONFIRMED=false
    USE_AIDL=true
  fi
fi

# ── Signal 2: Static FS — AIDL-positive ──────────────────────────────────────
if ! $USE_AIDL && ! $LEGACY_CONFIRMED; then
  # 2a. AIDL HAL binaries in vendor or APEX
  if ls /vendor/bin/hw/*audio*aidl* 1>/dev/null 2>&1 || \
     ls /apex/com.android.hardware.audio/bin/hw/*audio* 1>/dev/null 2>&1; then
    USE_AIDL=true
  fi
  # 2b. VINTF manifest declares AIDL audio effect interface
  if ! $USE_AIDL && grep -rq "android.hardware.audio.effect" /vendor/etc/vintf/ 2>/dev/null; then
    USE_AIDL=true
  fi
  # 2c. audio_effects_config.xml — AOSP AIDL-era filename (distinct from
  #     legacy audio_effects.conf and audio_effects.xml)
  if ! $USE_AIDL && [ -f "/vendor/etc/audio_effects_config.xml" ] && \
     ! [ -f "/vendor/etc/audio_effects.conf" ] && \
     ! [ -f "/vendor/etc/audio_effects.xml" ]; then
    USE_AIDL=true
  fi
fi

# ── Signal 3: Runtime property (booted mode only) ────────────────────────────
if ! $USE_AIDL && ! $LEGACY_CONFIRMED; then
  if [ "$(getprop init.svc.vendor.audio-hal-aidl 2>/dev/null)" = "running" ] || \
     [ "$(getprop init.svc.vendor.audio-effect-hal-aidl 2>/dev/null)" = "running" ]; then
    USE_AIDL=true
  fi
fi

# ── Signal 4: ps scan fallback ────────────────────────────────────────────────
if ! $USE_AIDL && ! $LEGACY_CONFIRMED && [ "$API" -ge 33 ]; then
  if ps -A 2>/dev/null | grep -qE '([Aa]udio[^[:space:]]*[Aa]idl|[Aa]idl[^[:space:]]*[Aa]udio)'; then
    USE_AIDL=true
  fi
fi

# ── Signal 5: API >= 35 last-resort tiebreaker ───────────────────────────────
# Only fires when ALL static and runtime evidence was absent (e.g. flashing on
# a freshly wiped device where /vendor is not yet populated with final OEM
# blobs). Skip entirely if legacy stack was confirmed above.
if ! $USE_AIDL && ! $LEGACY_CONFIRMED && [ "$API" -ge 35 ]; then
  ui_print "    ! No FS evidence found; defaulting to AIDL on API >= 35 (tiebreaker)"
  USE_AIDL=true
fi

# ── Helper: place_file SRC ORIG_DEVICE_PATH ───────────────────────────────────
# KernelSU auto-generates symlinks: $MODPATH/vendor → $MODPATH/system/vendor,
# $MODPATH/product → $MODPATH/system/product, etc. at install time (see KSU
# module.rs / installer.sh handle_partition()). Magisk magic-mount also reads
# from $MODPATH/system/. So writing ONLY to $MODPATH/system/<partition>/... is
# correct and sufficient for ALL root managers (Magisk, KSU, KSUNext, APatch).
# Writing bare $MODPATH/vendor/... is redundant — it resolves to the same inode.
place_file() {
  local SRC="$1"
  local ORIG_PATH="$2"
  local REL_PATH="${ORIG_PATH#/}"   # strip leading slash
  local PART="${REL_PATH%%/*}"      # first path component = partition name

  case "$PART" in
    vendor|product|system_ext|odm)
      # Always write under system/ — KSU symlinks bare partition dirs here anyway
      cp_ch -n "$SRC" "$MODPATH/system/$REL_PATH"
      ;;
    system)
      cp_ch -n "$SRC" "$MODPATH/$REL_PATH"
      ;;
    *)
      cp_ch -n "$SRC" "$MODPATH/system/$REL_PATH"
      ;;
  esac
}

if $USE_AIDL; then
  ui_print "    AIDL audio HAL detected — installing AIDL variant"
  echo "aidl" > "$MODPATH/aidl_mode.txt"
  sed -i 's/^name=.*/name=ViPER4Android Driver (AIDL)/' "$MODPATH/module.prop"

  ui_print "    Copying AIDL lib files..."
  # 32-bit: guard against pure-64 devices where ABI32 is empty or "null"
  if [ -n "$ABI32" ] && [ "$ABI32" != "null" ] && \
     [ -f "$MODPATH/common/files/libv4a_aidl_$ABI32.so" ]; then
    place_file "$MODPATH/common/files/libv4a_aidl_$ABI32.so" "/vendor/lib/soundfx/libv4a_aidl.so"
  fi
  # 64-bit
  if $IS64BIT && [ -f "$MODPATH/common/files/libv4a_aidl_$ABI.so" ]; then
    place_file "$MODPATH/common/files/libv4a_aidl_$ABI.so" "/vendor/lib64/soundfx/libv4a_aidl.so"
  fi

  ui_print "    Patching audio effect config files (AIDL)..."
  # Use \( ... \) so -o is part of the -type f sub-expression, not a top-level OR
  CFGS="$(find /odm /system /vendor /product /system_ext -type f \
    \( -name "*audio_effects*.conf" \
    -o -name "*audio_effects*.xml" \
    -o -name "*audio_effects_config*.xml" \) 2>/dev/null)"

  for OFILE in ${CFGS}; do
    TMP_FILE="$TMPDIR/v4a_$(basename "$OFILE")"
    cp -f "$OFILE" "$TMP_FILE"
    case "$TMP_FILE" in
      *.conf)
        sed -i "/v4a_standard_re {/,/}/d" "$TMP_FILE"
        sed -i "/v4a_aidl {/,/}/d" "$TMP_FILE"
        sed -i "s/^[[:space:]]*effects[[:space:]]*{/effects {\n  v4a_standard_re {\n    library v4a_aidl\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$TMP_FILE"
        sed -i "s/^[[:space:]]*libraries[[:space:]]*{/libraries {\n  v4a_aidl {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_aidl.so\n  }/g" "$TMP_FILE"
        ;;
      *audio_effects_config*.xml)
        sed -i "/v4a_standard_re/d" "$TMP_FILE"
        sed -i "/v4a_standard_aidl/d" "$TMP_FILE"
        sed -i "/v4a_aidl/d" "$TMP_FILE"
        sed -i "/<libraries/ a\\        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"\/>" "$TMP_FILE"
        sed -i "/<effects/ a\\        <effect name=\"v4a_standard_aidl\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"\/>" "$TMP_FILE"
        ;;
      *.xml)
        sed -i "/v4a_standard_re/d" "$TMP_FILE"
        sed -i "/v4a_standard_aidl/d" "$TMP_FILE"
        sed -i "/v4a_aidl/d" "$TMP_FILE"
        sed -i "/<libraries/ a\\        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"\/>" "$TMP_FILE"
        sed -i "/<effects/ a\\        <effect name=\"v4a_standard_re\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"\/>" "$TMP_FILE"
        ;;
    esac
    place_file "$TMP_FILE" "$OFILE"
    rm -f "$TMP_FILE"
  done

else
  ui_print "    Legacy audio HAL detected — installing Legacy variant"
  echo "legacy" > "$MODPATH/aidl_mode.txt"
  sed -i 's/^name=.*/name=ViPER4Android Driver (Legacy)/' "$MODPATH/module.prop"

  ui_print "    Copying Legacy lib files..."
  # 32-bit guard
  if [ -n "$ABI32" ] && [ "$ABI32" != "null" ] && \
     [ -f "$MODPATH/common/files/libv4a_re_$ABI32.so" ]; then
    place_file "$MODPATH/common/files/libv4a_re_$ABI32.so" "/vendor/lib/soundfx/libv4a_re.so"
  fi
  # 64-bit
  if $IS64BIT && [ -f "$MODPATH/common/files/libv4a_re_$ABI.so" ]; then
    place_file "$MODPATH/common/files/libv4a_re_$ABI.so" "/vendor/lib64/soundfx/libv4a_re.so"
  fi

  ui_print "    Patching audio effect config files..."
  CFGS="$(find /odm /system /vendor /product /system_ext -type f \
    \( -name "*audio_effects*.conf" \
    -o -name "*audio_effects*.xml" \) 2>/dev/null)"

  for OFILE in ${CFGS}; do
    TMP_FILE="$TMPDIR/v4a_$(basename "$OFILE")"
    cp -f "$OFILE" "$TMP_FILE"
    case "$TMP_FILE" in
      *.conf)
        sed -i "/v4a_standard_re {/,/}/d" "$TMP_FILE"
        sed -i "/v4a_re {/,/}/d" "$TMP_FILE"
        sed -i "s/^[[:space:]]*effects[[:space:]]*{/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$TMP_FILE"
        sed -i "s/^[[:space:]]*libraries[[:space:]]*{/libraries {\n  v4a_re {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_re.so\n  }/g" "$TMP_FILE"
        ;;
      *.xml)
        sed -i "/v4a_standard_re/d" "$TMP_FILE"
        sed -i "/v4a_re/d" "$TMP_FILE"
        sed -i "/<libraries/ a\\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" "$TMP_FILE"
        sed -i "/<effects/ a\\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" "$TMP_FILE"
        ;;
    esac
    place_file "$TMP_FILE" "$OFILE"
    rm -f "$TMP_FILE"
  done
fi
