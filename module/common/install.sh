# ── HAL type detection — symmetric AIDL + HIDL probing ───────────────────────
#
# Design: run BOTH AIDL and HIDL detectors independently, then resolve.
#
# AIDL signals (highest confidence first):
#   A1. Binder IFactory service registered — hard runtime proof.
#   A2. AIDL HAL binary in /vendor/bin/hw/ or com.android.hardware.audio APEX.
#   A3. Vendor VINTF manifest has <hal format="aidl"> for audio.effect.
#   A4. audio_effects_config.xml present without legacy audio_effects.conf/xml.
#   A5. Running AIDL HAL service name (getprop / ps scan).
#
# HIDL signals (highest confidence first):
#   H1. Binder IEffectsFactory HIDL service registered — hard runtime proof.
#   H2. libeffectproxy.so present — the HIDL effects proxy, absent on AIDL stacks.
#   H3. audio_effects.conf or audio_effects.xml present in /vendor/etc/.
#   H4. Vendor VINTF manifest has <hal format="hidl"> for audio.effect.
#   H5. HIDL IEffectsFactory service in hwservicemanager (service list).
#
# Resolution:
#   - If BOTH are detected: AIDL wins (a device mid-migration keeps HIDL shims).
#   - If only AIDL:  USE_AIDL=true.
#   - If only HIDL:  USE_AIDL=false (legacy).
#   - If neither:    safe default → legacy.
#
# /system/etc/vintf/ is NEVER searched for either detector: it holds the
# framework *compatibility matrix* — what Android *accepts*, not what the
# vendor *provides*.  It matches both AIDL and HIDL names on every A13+ device
# and is useless for HAL presence detection.
ui_print "- Detecting audio HAL type..."

# ── Shared helper: awk-based <hal> block scanner ─────────────────────────────
# vintf_hal_format HAL_NAME FORMAT  →  returns 0 if vendor VINTF has a <hal>
# block with <name>HAL_NAME</name> and format="FORMAT" attribute.
# Searches /vendor/etc/vintf/, /vendor/manifest.xml, /odm/etc/vintf/ only.
vintf_hal_format() {
  local name="$1" fmt="$2"
  grep -rl "$name" \
       /vendor/etc/vintf/ /vendor/manifest.xml \
       /odm/etc/vintf/ 2>/dev/null | \
  while read -r f; do
    awk -v name="$name" -v fmt="$fmt" '
      /<hal/{in_hal=1; has_fmt=0; has_name=0}
      in_hal && $0 ~ ("format=\"" fmt "\""){has_fmt=1}
      in_hal && $0 ~ name {has_name=1}
      in_hal && /<\/hal>/{
        if(has_fmt && has_name){found=1; exit}
        in_hal=0
      }
      END{exit !found}
    ' "$f" 2>/dev/null && return 0
  done
  return 1
}

AIDL_SCORE=0
HIDL_SCORE=0

# ════════════════════════════════════════════════════════════════════════════
# AIDL probes
# ════════════════════════════════════════════════════════════════════════════

# A1 — lshal shows AIDL IFactory (strongest possible signal).
# AIDL vendor HALs appear in lshal WITHOUT an @version, e.g.:
#   "android.hardware.audio.effect.IFactory/default"
# IMPORTANT: Do NOT use "service check android.hardware.audio.effect.IFactory" here.
# On Android 13+, audioserver registers its own in-process AIDL IFactory proxy in
# the regular ServiceManager even when the vendor HAL underneath is pure HIDL.
# That "service check" would fire on every Android 13+ device → guaranteed false-positive.
if lshal 2>/dev/null | grep -qF "android.hardware.audio.effect.IFactory"; then
  ui_print "    [AIDL A1] lshal: AIDL IFactory vendor HAL confirmed"
  AIDL_SCORE=$((AIDL_SCORE + 10))
fi

# A2 — AIDL HAL binary in vendor or APEX
if ls /vendor/bin/hw/*audio*aidl* 1>/dev/null 2>&1 || \
   ls /apex/com.android.hardware.audio/bin/hw/*audio* 1>/dev/null 2>&1; then
  ui_print "    [AIDL A2] AIDL HAL binary found"
  AIDL_SCORE=$((AIDL_SCORE + 4))
fi

# A3 — Vendor VINTF declares <hal format="aidl"> for android.hardware.audio.effect
if vintf_hal_format "android.hardware.audio.effect" "aidl"; then
  ui_print "    [AIDL A3] Vendor VINTF declares AIDL audio effect HAL"
  AIDL_SCORE=$((AIDL_SCORE + 4))
fi

# A4 — audio_effects_config.xml present without any legacy effects file
if [ -f "/vendor/etc/audio_effects_config.xml" ] && \
   ! [ -f "/vendor/etc/audio_effects.conf" ] && \
   ! [ -f "/vendor/etc/audio_effects.xml" ]; then
  ui_print "    [AIDL A4] AIDL-era audio_effects_config.xml found"
  AIDL_SCORE=$((AIDL_SCORE + 2))
fi

# A5 — Running AIDL service name (getprop) or ps scan
if [ "$(getprop init.svc.vendor.audio-hal-aidl 2>/dev/null)" = "running" ] || \
   [ "$(getprop init.svc.vendor.audio-effect-hal-aidl 2>/dev/null)" = "running" ]; then
  ui_print "    [AIDL A5] AIDL HAL service property running"
  AIDL_SCORE=$((AIDL_SCORE + 3))
elif [ "$API" -ge 33 ] && \
     ps -A 2>/dev/null | grep -qE '([Aa]udio[^[:space:]]*[Aa]idl|[Aa]idl[^[:space:]]*[Aa]udio)'; then
  ui_print "    [AIDL A5] AIDL audio daemon found in ps"
  AIDL_SCORE=$((AIDL_SCORE + 2))
fi

# ════════════════════════════════════════════════════════════════════════════
# HIDL probes
# ════════════════════════════════════════════════════════════════════════════

# H1 — lshal shows HIDL IEffectsFactory (strongest HIDL signal).
# HIDL vendor HALs appear in lshal WITH an @version, e.g.:
#   "android.hardware.audio.effect@7.0::IEffectsFactory/default"
# IMPORTANT: Do NOT use "service list" here — on Android 12+, service list only
# queries the regular Binder ServiceManager and never sees hwbinder/HIDL services.
if lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect@[0-9]+\.[0-9]+::IEffectsFactory"; then
  ui_print "    [HIDL H1] lshal: HIDL IEffectsFactory vendor HAL confirmed"
  HIDL_SCORE=$((HIDL_SCORE + 10))
fi

# H2 — libeffectproxy.so: the HIDL effects pipeline proxy; absent on pure-AIDL stacks
if ls /vendor/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1 || \
   ls /system/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1; then
  ui_print "    [HIDL H2] libeffectproxy.so found (HIDL effects proxy)"
  HIDL_SCORE=$((HIDL_SCORE + 4))
fi

# H3 — Legacy effect config files
if ls /vendor/etc/audio_effects.conf 1>/dev/null 2>&1 || \
   ls /vendor/etc/audio_effects.xml 1>/dev/null 2>&1; then
  ui_print "    [HIDL H3] Legacy audio_effects config found"
  HIDL_SCORE=$((HIDL_SCORE + 3))
fi

# H4 — Vendor VINTF declares <hal format="hidl"> for android.hardware.audio.effect
if vintf_hal_format "android.hardware.audio.effect" "hidl"; then
  ui_print "    [HIDL H4] Vendor VINTF declares HIDL audio effect HAL"
  HIDL_SCORE=$((HIDL_SCORE + 4))
fi

# H5 — lshal fallback: any android.hardware.audio.effect@* entry (handles OEM renames)
if [ "$HIDL_SCORE" -eq 0 ] && \
   lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect@[0-9]"; then
  ui_print "    [HIDL H5] lshal: HIDL audio effect HAL entry found"
  HIDL_SCORE=$((HIDL_SCORE + 2))
fi

# ════════════════════════════════════════════════════════════════════════════
# Resolution
# ════════════════════════════════════════════════════════════════════════════
ui_print "    AIDL score=$AIDL_SCORE  HIDL score=$HIDL_SCORE"

USE_AIDL=false
if [ "$AIDL_SCORE" -gt 0 ] && [ "$AIDL_SCORE" -ge "$HIDL_SCORE" ]; then
  # AIDL evidence is present and at least as strong as HIDL evidence.
  # Even if HIDL shims exist (mid-migration device), AIDL wins.
  USE_AIDL=true
elif [ "$HIDL_SCORE" -gt 0 ]; then
  # Explicit HIDL evidence with no competing AIDL evidence → legacy.
  USE_AIDL=false
else
  # No evidence for either — safe default: legacy.
  # Wrongly-installed AIDL on a legacy device crashes AudioEffect (versionCode=-1);
  # wrongly-installed legacy on an AIDL device silently fails to load.
  # Failing silently is always safer.
  ui_print "    ! No HAL evidence found; defaulting to Legacy (safe fallback)"
  USE_AIDL=false
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

  # ── Shared patch helper ───────────────────────────────────────────────────
  # patch_aidl_cfg TMPFILE
  # Injects the v4a_aidl library + effect entry into a working copy of an
  # AIDL-era audio effect config file.  Call AFTER cp-ing the original.
  patch_aidl_cfg() {
    local f="$1"
    case "$f" in
      *.conf)
        sed -i "/v4a_standard_re {/,/}/d" "$f"
        sed -i "/v4a_aidl {/,/}/d" "$f"
        sed -i "s/^[[:space:]]*effects[[:space:]]*{/effects {\n  v4a_standard_re {\n    library v4a_aidl\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$f"
        sed -i "s/^[[:space:]]*libraries[[:space:]]*{/libraries {\n  v4a_aidl {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_aidl.so\n  }/g" "$f"
        ;;
      *audio_effects_config*.xml | *.xml)
        sed -i "/v4a_standard_re/d"   "$f"
        sed -i "/v4a_standard_aidl/d" "$f"
        sed -i "/v4a_aidl/d"          "$f"
        # Expand self-closing tags before insertion: sed "a" appends after the
        # opening tag line, but silently does nothing when the tag is self-closing
        # (e.g. <libraries/> or <effects/>), leaving the config un-patched.
        sed -i 's|<libraries/>|<libraries>\n</libraries>|g' "$f"
        sed -i 's|<effects/>|<effects>\n</effects>|g' "$f"
        sed -i "/<libraries/ a\\        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"\/>" "$f"
        sed -i "/<effects/ a\\        <effect name=\"v4a_standard_aidl\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261676f-6d75-7369-6364-28e2fd3ac39e\"\/>" "$f"
        ;;
    esac
  }

  ui_print "    Patching audio effect config files (AIDL — vendor partitions)..."
  # Use \( ... \) so -o is part of the -type f sub-expression, not a top-level OR
  CFGS="$(find /odm /system /vendor /product /system_ext -type f \
    \( -name "*audio_effects*.conf" \
    -o -name "*audio_effects*.xml" \
    -o -name "*audio_effects_config*.xml" \) 2>/dev/null)"

  for OFILE in ${CFGS}; do
    TMP_FILE="$TMPDIR/v4a_$(basename "$OFILE")"
    cp -f "$OFILE" "$TMP_FILE"
    patch_aidl_cfg "$TMP_FILE"
    place_file "$TMP_FILE" "$OFILE"
    rm -f "$TMP_FILE"
  done

  # ── APEX config injection ─────────────────────────────────────────────────
  # On devices where IFactory runs inside an APEX (e.g. Pixel 8 / AOSP
  # com.android.hardware.audio.effect), EffectConfig.cpp calls
  # config_file_path() which returns /apex/<name>/etc/audio_effects_config.xml
  # — NOT /vendor/etc/.  The vendor-partition patches above are skipped.
  #
  # EffectConfig::resolveLibrary() DOES fall through to /vendor/lib64/soundfx/
  # when the .so is absent from the APEX lib dir, so libv4a_aidl.so placement
  # at /vendor/lib64/soundfx/ is already correct.  Only the XML registration
  # is missing on APEX devices.
  #
  # Fix: patch the APEX config and store it under $MODPATH/apex/<name>/etc/.
  # post-fs-data.sh bind-mounts each such file over the live APEX path at boot,
  # before audioserver starts.
  ui_print "    Patching audio effect config files (AIDL — APEX)..."
  for APEX_DIR in /apex/*/; do
    APEX_NAME="$(basename "$APEX_DIR")"
    APEX_CFG="$APEX_DIR/etc/audio_effects_config.xml"
    [ -f "$APEX_CFG" ] || continue
    ui_print "      Found APEX config: $APEX_CFG"
    TMP_FILE="$TMPDIR/v4a_apex_$(echo "$APEX_NAME" | tr '.' '_').xml"
    cp -f "$APEX_CFG" "$TMP_FILE"
    patch_aidl_cfg "$TMP_FILE"
    # Store under $MODPATH/apex/<name>/etc/ for post-fs-data.sh to bind-mount
    DEST_DIR="$MODPATH/apex/$APEX_NAME/etc"
    mkdir -p "$DEST_DIR"
    cp -f "$TMP_FILE" "$DEST_DIR/audio_effects_config.xml"
    rm -f "$TMP_FILE"
    ui_print "      Stored patched APEX config for $APEX_NAME"
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
        # Expand self-closing tags before insertion (same reason as AIDL branch).
        sed -i 's|<libraries/>|<libraries>\n</libraries>|g' "$TMP_FILE"
        sed -i 's|<effects/>|<effects>\n</effects>|g' "$TMP_FILE"
        sed -i "/<libraries/ a\\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" "$TMP_FILE"
        sed -i "/<effects/ a\\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" "$TMP_FILE"
        ;;
    esac
    place_file "$TMP_FILE" "$OFILE"
    rm -f "$TMP_FILE"
  done
fi
