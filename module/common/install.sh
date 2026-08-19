# ── AIDL detection ────────────────────────────────────────────────────────────
# Four signals, any one is sufficient:
#   1. API >= 35: Android 15 removed the legacy audio effect HAL entirely.
#      Always use AIDL on API 35+.
#   2. init.svc.vendor.audio-hal-aidl = "running" or
#      init.svc.vendor.audio-effect-hal-aidl = "running"
#      AOSP-standardized service names (Android 14+). Zero-cost getprop.
#   3. ps -A fallback: catches OEM binaries whose service name differs but
#      whose binary path still contains both "audio" and "aidl" in either order.
#   4. Static filesystem inspection: works in recovery / offline mode where
#      no properties or processes exist yet.
ui_print "- Detecting audio HAL type..."
USE_AIDL=false

# Signal 1: API >= 35 — legacy HAL was removed in AOSP Android 15
if [ "$API" -ge 35 ]; then
  USE_AIDL=true
fi

# Signal 2: O(1) property lookup — most reliable when booted
if ! $USE_AIDL; then
  if [ "$(getprop init.svc.vendor.audio-hal-aidl 2>/dev/null)" = "running" ] || \
     [ "$(getprop init.svc.vendor.audio-effect-hal-aidl 2>/dev/null)" = "running" ]; then
    USE_AIDL=true
  fi
fi

# Signal 3: ps scan fallback (only if signals 1/2 missed and device is recent)
if ! $USE_AIDL && [ "$API" -ge 33 ]; then
  if ps -A 2>/dev/null | grep -qE '([Aa]udio[^[:space:]]*[Aa]idl|[Aa]idl[^[:space:]]*[Aa]udio)'; then
    USE_AIDL=true
  fi
fi

# Signal 4: Static FS inspection — safe in TWRP/OrangeFox recovery and offline mode
if ! $USE_AIDL && [ "$API" -ge 33 ]; then
  # AIDL HAL binaries
  if ls /vendor/bin/hw/*audio*aidl* 1>/dev/null 2>&1 || \
     ls /vendor/bin/hw/*audio*effect* 1>/dev/null 2>&1 || \
     ls /apex/com.android.hardware.audio/bin/*aidl* 1>/dev/null 2>&1; then
    USE_AIDL=true
  fi
  # VINTF manifest declares AIDL audio effect interface
  if ! $USE_AIDL && grep -rq "android.hardware.audio.effect" /vendor/etc/vintf/ 2>/dev/null; then
    USE_AIDL=true
  fi
  # audio_effects_config.xml is the standard AIDL effect configuration filename
  if ! $USE_AIDL && [ -f "/vendor/etc/audio_effects_config.xml" ]; then
    USE_AIDL=true
  fi
fi

# ── Helper: place_file SRC ORIG_DEVICE_PATH ───────────────────────────────────
# Writes the patched file to BOTH the bare partition path (for KernelSU/APatch
# OverlayFS) AND under system/ (for Magisk magic-mount). This ensures every
# root manager variant picks up the patched file correctly.
place_file() {
  local SRC="$1"
  local ORIG_PATH="$2"
  local REL_PATH="${ORIG_PATH#/}"   # strip leading slash
  local PART="${REL_PATH%%/*}"      # first path component = partition name

  case "$PART" in
    vendor|product|system_ext|odm)
      # KernelSU/APatch OverlayFS mounts partitions at /vendor, /product, etc.
      cp_ch -n "$SRC" "$MODPATH/$REL_PATH"
      # Magisk magic-mount expects them under /system/
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
