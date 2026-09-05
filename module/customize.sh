SKIPUNZIP=1

V4A_LOG=/data/adb/viper_install.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [install] $1" >> "$V4A_LOG"; }

echo "" > "$V4A_LOG"
log "---> ViPER4Android Installation Started <---"

ui_print "**************************************"
ui_print "*      ViPER4Android RE Installer     *"
ui_print "**************************************"
ui_print " "

log "Device: $(getprop ro.product.brand) $(getprop ro.product.model)"
log "ROM: $(getprop ro.build.display.id)"
log "Android: $(getprop ro.build.version.release) (API $API)"
log "Arch: $ARCH (ABI: $ABI, ABI32: ${ABI32:-none})"
log "Kernel: $(uname -r)"

ROOT_TYPE="unknown"
if [ -n "$KSU" ]; then
  ROOT_TYPE="KernelSU $(ksud --version 2>/dev/null || echo unknown)"
elif [ -n "$APATCH" ]; then
  ROOT_TYPE="APatch $(apd --version 2>/dev/null || echo unknown)"
elif [ -n "$MAGISK_VER" ]; then
  ROOT_TYPE="Magisk $MAGISK_VER ($MAGISK_VER_CODE)"
else
  ROOT_TYPE="su: $(which su 2>/dev/null || echo not found)"
fi
log "Root manager: $ROOT_TYPE"
log "SELinux: $(getenforce 2>/dev/null || echo unknown)"
log "ro.audio.ignore_effects: $(getprop ro.audio.ignore_effects)"
log "ro.vendor.audio.effect.type: $(getprop ro.vendor.audio.effect.type)"
log "persist.vendor.audio.effects.enable: $(getprop persist.vendor.audio.effects.enable)"

if ! $BOOTMODE; then
  ui_print "! Recovery installation is not supported."
  ui_print "! Please install via Magisk, KernelSU, or APatch app."
  abort "! Aborting installation"
fi

ui_print "- Extracting module files..."
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2

# ── HAL Type Detection: AOSP-Aligned Symmetric AIDL vs HIDL Probing ─────────
ui_print "- Detecting audio HAL architecture..."
log "Starting HAL architecture detection..."

# Tier 0: Check manual overrides & hard OS boundaries
OVERRIDE_MODE=""
if [ -f /data/local/tmp/v4a_force_aidl ]; then
  OVERRIDE_MODE="aidl"
  ui_print "    [OVERRIDE] /data/local/tmp/v4a_force_aidl detected -> Forcing AIDL"
  log "Manual override: force AIDL"
elif [ -f /data/local/tmp/v4a_force_legacy ]; then
  OVERRIDE_MODE="legacy"
  ui_print "    [OVERRIDE] /data/local/tmp/v4a_force_legacy detected -> Forcing Legacy"
  log "Manual override: force Legacy"
fi

if [ "$API" -le 32 ]; then
  ui_print "    Android $API (<= 32) lacks AIDL audio HAL support -> Using Legacy"
  log "API $API <= 32: AIDL non-existent on this Android release; enforcing Legacy"
  USE_AIDL=false
  OVERRIDE_MODE="legacy"
fi

AIDL_SCORE=0
HIDL_SCORE=0

# Helper: Scans VINTF manifests across vendor, odm, and apex directories
# Mimics AOSP FactoryHal.cpp hasAidlHalService() -> AServiceManager_isDeclared()
vintf_manifest_hal_format() {
  local hal_name="$1" fmt="$2"
  local files
  files="$(grep -rl "$hal_name" /vendor/etc/vintf/ /vendor/manifest.xml \
           /odm/etc/vintf/ /odm/manifest.xml /apex/*/etc/vintf/ \
           /my_product/etc/vintf/ /vivo_product/etc/vintf/ \
           /system/etc/vintf/manifest.xml 2>/dev/null)"
  [ -z "$files" ] && return 1
  for f in $files; do
    # Skip framework compatibility matrices (they declare framework acceptance, not vendor provision)
    case "$f" in
      *compatibility_matrix*) continue ;;
    esac
    if awk -v name="$hal_name" -v fmt="$fmt" '
      /<hal/{in_hal=1; has_fmt=0; has_name=0}
      in_hal && $0 ~ ("format=[\\\"\\\x27]" fmt "[\\\"\\\x27]"){has_fmt=1}
      in_hal && $0 ~ name {has_name=1}
      in_hal && /<\/hal>/{
        if(has_fmt && has_name){found=1; exit 0}
        in_hal=0
      }
      END{exit !found}
    ' "$f" 2>/dev/null; then
      return 0
    fi
  done
  return 1
}

# ── Tier 1: Definitive Runtime Service Checks (AOSP FactoryHal alignment) ────
# In AOSP FactoryHal.cpp, hasAidlHalService() checks AServiceManager_isDeclared()
# for "android.hardware.audio.effect.IFactory/default" and "android.hardware.audio.core.IModule/default".
if service check android.hardware.audio.effect.IFactory/default 2>/dev/null | grep -qi "found"; then
  ui_print "    [AIDL T1] ServiceManager: android.hardware.audio.effect.IFactory/default confirmed (+20)"
  AIDL_SCORE=$((AIDL_SCORE + 20))
elif lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect\.IFactory"; then
  ui_print "    [AIDL T1] lshal: AIDL IFactory vendor HAL confirmed (+20)"
  AIDL_SCORE=$((AIDL_SCORE + 20))
fi

if service check android.hardware.audio.core.IModule/default 2>/dev/null | grep -qi "found"; then
  ui_print "    [AIDL T1] ServiceManager: android.hardware.audio.core.IModule/default confirmed (+15)"
  AIDL_SCORE=$((AIDL_SCORE + 15))
elif lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.core\.IModule"; then
  ui_print "    [AIDL T1] lshal: AIDL core IModule vendor HAL confirmed (+15)"
  AIDL_SCORE=$((AIDL_SCORE + 15))
fi

# HIDL service checks in hwservicemanager
if lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect@[0-9]+\.[0-9]+::IEffectsFactory"; then
  ui_print "    [HIDL T1] lshal: HIDL IEffectsFactory vendor HAL confirmed (+20)"
  HIDL_SCORE=$((HIDL_SCORE + 20))
fi

if lshal 2>/dev/null | grep -qE "android\.hardware\.audio@[0-9]+\.[0-9]+::IDevicesFactory"; then
  ui_print "    [HIDL T1] lshal: HIDL IDevicesFactory vendor HAL confirmed (+15)"
  HIDL_SCORE=$((HIDL_SCORE + 15))
fi

# ── Tier 2: VINTF Manifest Declarations ──────────────────────────────────────
if vintf_manifest_hal_format "android.hardware.audio.effect" "aidl"; then
  ui_print "    [AIDL T2] VINTF manifest: declares AIDL audio.effect HAL (+15)"
  AIDL_SCORE=$((AIDL_SCORE + 15))
fi

if vintf_manifest_hal_format "android.hardware.audio.core" "aidl"; then
  ui_print "    [AIDL T2] VINTF manifest: declares AIDL audio.core HAL (+10)"
  AIDL_SCORE=$((AIDL_SCORE + 10))
fi

if vintf_manifest_hal_format "android.hardware.audio.effect" "hidl"; then
  ui_print "    [HIDL T2] VINTF manifest: declares HIDL audio.effect HAL (+15)"
  HIDL_SCORE=$((HIDL_SCORE + 15))
fi

if vintf_manifest_hal_format "android.hardware.audio" "hidl"; then
  ui_print "    [HIDL T2] VINTF manifest: declares HIDL audio HAL (+10)"
  HIDL_SCORE=$((HIDL_SCORE + 10))
fi

# ── Tier 3: Active HAL Daemons and Service Properties ────────────────────────
AIDL_SVC_RUNNING=false
for prop in \
  init.svc.vendor.audio-hal-aidl \
  init.svc.vendor.audio-effect-hal-aidl \
  init.svc.audio-hal-aidl \
  init.svc.secaudiohalaidl \
  init.svc.audio.service-aidl.mediatek \
  init.svc.vendor.qti.hardware.audio.service-aidl \
  init.svc.vendor.oplus.hardware.audio.service \
  init.svc.vendor.vivo.hardware.audio.service \
  init.svc.vendor.mediatek.hardware.audio.service; do
  if [ "$(getprop "$prop" 2>/dev/null)" = "running" ]; then
    AIDL_SVC_RUNNING=true
    break
  fi
done

if $AIDL_SVC_RUNNING; then
  ui_print "    [AIDL T3] Running AIDL audio HAL init service detected (+10)"
  AIDL_SCORE=$((AIDL_SCORE + 10))
elif [ "$API" -ge 33 ] && ps -A 2>/dev/null | grep -iE '([Aa]udio.*[Aa]idl|[Aa]idl.*[Aa]udio|secaudiohalaidl|audio\.service-aidl|oplus\.hardware\.audio|vivo\.hardware\.audio)'; then
  ui_print "    [AIDL T3] Running AIDL audio daemon process found in ps (+8)"
  AIDL_SCORE=$((AIDL_SCORE + 8))
fi

HIDL_SVC_RUNNING=false
for prop in \
  init.svc.vendor.audio-hal-2-0 \
  init.svc.vendor.audio-hal-4-0 \
  init.svc.vendor.audio-hal-5-0 \
  init.svc.vendor.audio-hal-6-0 \
  init.svc.vendor.audio-hal-7-0 \
  init.svc.vendor.audio-hal-7-1; do
  if [ "$(getprop "$prop" 2>/dev/null)" = "running" ]; then
    HIDL_SVC_RUNNING=true
    break
  fi
done

if $HIDL_SVC_RUNNING; then
  ui_print "    [HIDL T3] Running HIDL audio HAL init service detected (+10)"
  HIDL_SCORE=$((HIDL_SCORE + 10))
elif ps -A 2>/dev/null | grep -qE 'android\.hardware\.audio@[0-9]\.[0-9]-service'; then
  ui_print "    [HIDL T3] Running HIDL audio daemon process found in ps (+8)"
  HIDL_SCORE=$((HIDL_SCORE + 8))
fi

# ── Tier 4: Hardware Binaries and Library Artifacts ──────────────────────────
if ls /vendor/bin/hw/*audio*aidl* 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/*audio*mediatek* 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/secaudiohalaidl 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/*oplus*audio* 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/*vivo*audio* 1>/dev/null 2>&1 || \
   ls /apex/com.android.hardware.audio/bin/hw/* 1>/dev/null 2>&1 || \
   ls /apex/com.android.hardware.audio.effect/bin/hw/* 1>/dev/null 2>&1; then
  ui_print "    [AIDL T4] AIDL HAL binary found in /vendor/bin/hw/ or APEX (+6)"
  AIDL_SCORE=$((AIDL_SCORE + 6))
fi

if ls /vendor/bin/hw/android.hardware.audio@*-service 1>/dev/null 2>&1; then
  ui_print "    [HIDL T4] HIDL HAL binary found in /vendor/bin/hw/ (+6)"
  HIDL_SCORE=$((HIDL_SCORE + 6))
fi

if ls /vendor/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1 || \
   ls /system/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1; then
  ui_print "    [HIDL T4] libeffectproxy.so found (exclusive to HIDL pipeline) (+6)"
  HIDL_SCORE=$((HIDL_SCORE + 6))
fi

# ── Tier 5: Audio Effect Configuration Signature ─────────────────────────────
if [ -f "/vendor/etc/audio_effects_config.xml" ] || [ -f "/system/etc/audio_effects_config.xml" ]; then
  if ! [ -f "/vendor/etc/audio_effects.conf" ] && ! [ -f "/system/etc/audio_effects.conf" ]; then
    ui_print "    [AIDL T5] Pure audio_effects_config.xml structure found (+4)"
    AIDL_SCORE=$((AIDL_SCORE + 4))
  fi
fi

if ls /vendor/etc/audio_effects.conf 1>/dev/null 2>&1 || \
   ls /system/etc/audio_effects.conf 1>/dev/null 2>&1 || \
   ls /odm/etc/audio_effects.conf 1>/dev/null 2>&1; then
  ui_print "    [HIDL T5] Legacy audio_effects.conf found (+6)"
  HIDL_SCORE=$((HIDL_SCORE + 6))
fi

# ── Tier 6: Android OS Prior (AOSP Version Baseline) ─────────────────────────
# Android 15 (API 35+) deprecated and removed HIDL audio HAL support.
if [ "$API" -ge 35 ]; then
  ui_print "    [PRIOR] Android 15+ (API $API) mandates AIDL audio HAL (+12)"
  AIDL_SCORE=$((AIDL_SCORE + 12))
elif [ "$API" -eq 34 ]; then
  ui_print "    [PRIOR] Android 14 (API 34) defaults to AIDL audio HAL (+4)"
  AIDL_SCORE=$((AIDL_SCORE + 4))
fi

ui_print "    Calculated Scores: AIDL=$AIDL_SCORE | HIDL=$HIDL_SCORE (API $API)"
log "Final HAL Scores: AIDL=$AIDL_SCORE, HIDL=$HIDL_SCORE, API=$API, Override=$OVERRIDE_MODE"

# ── Final Resolution Aligned with AOSP FactoryHal Priority ───────────────────
# In AOSP FactoryHal.cpp, sAudioHALVersions prioritizes AIDL over HIDL.
if [ -n "$OVERRIDE_MODE" ]; then
  [ "$OVERRIDE_MODE" = "aidl" ] && USE_AIDL=true || USE_AIDL=false
  ui_print "    -> Decision: Enforced by manual override ($OVERRIDE_MODE)"
elif [ "$AIDL_SCORE" -gt 0 ] && [ "$AIDL_SCORE" -ge "$HIDL_SCORE" ]; then
  USE_AIDL=true
  ui_print "    -> Decision: AIDL audio HAL selected (score $AIDL_SCORE >= $HIDL_SCORE)"
elif [ "$API" -ge 35 ] && [ "$HIDL_SCORE" -lt 20 ]; then
  # On Android 15+, unless there is definitive proof of a running HIDL service (>= 20), AIDL is selected
  USE_AIDL=true
  ui_print "    -> Decision: AIDL audio HAL selected (Android 15+ requirement)"
elif [ "$HIDL_SCORE" -gt "$AIDL_SCORE" ]; then
  USE_AIDL=false
  ui_print "    -> Decision: Legacy (HIDL) audio HAL selected (score $HIDL_SCORE > $AIDL_SCORE)"
elif [ "$API" -ge 34 ]; then
  USE_AIDL=true
  ui_print "    -> Decision: AIDL audio HAL selected (Android 14+ default)"
else
  USE_AIDL=false
  ui_print "    -> Decision: Legacy (HIDL) audio HAL selected (safe fallback)"
fi

# ── Library Installation ───────────────────────────────────────────────────
mkdir -p "$MODPATH/system/vendor/lib/soundfx"
mkdir -p "$MODPATH/system/vendor/lib64/soundfx"

if $USE_AIDL; then
  ui_print "- Installing AIDL driver variant..."
  echo "aidl" > "$MODPATH/aidl_mode.txt"
  sed -i 's/^name=.*/name=ViPER4Android Driver (AIDL)/' "$MODPATH/module.prop"
  log "Selected mode: AIDL"

  if [ -f "$MODPATH/common/files/libv4a_aidl_armeabi-v7a.so" ]; then
    cp -f "$MODPATH/common/files/libv4a_aidl_armeabi-v7a.so" "$MODPATH/system/vendor/lib/soundfx/libv4a_aidl.so"
    ui_print "  Installed 32-bit AIDL library"
    log "Installed 32-bit AIDL library"
  fi
  if [ -f "$MODPATH/common/files/libv4a_aidl_arm64-v8a.so" ]; then
    cp -f "$MODPATH/common/files/libv4a_aidl_arm64-v8a.so" "$MODPATH/system/vendor/lib64/soundfx/libv4a_aidl.so"
    ui_print "  Installed 64-bit AIDL library"
    log "Installed 64-bit AIDL library"
  fi
else
  ui_print "- Installing Legacy (HIDL) driver variant..."
  echo "legacy" > "$MODPATH/aidl_mode.txt"
  sed -i 's/^name=.*/name=ViPER4Android Driver (Legacy)/' "$MODPATH/module.prop"
  log "Selected mode: Legacy"

  if [ -f "$MODPATH/common/files/libv4a_re_armeabi-v7a.so" ]; then
    cp -f "$MODPATH/common/files/libv4a_re_armeabi-v7a.so" "$MODPATH/system/vendor/lib/soundfx/libv4a_re.so"
    ui_print "  Installed 32-bit Legacy library"
    log "Installed 32-bit Legacy library"
  fi
  if [ -f "$MODPATH/common/files/libv4a_re_arm64-v8a.so" ]; then
    cp -f "$MODPATH/common/files/libv4a_re_arm64-v8a.so" "$MODPATH/system/vendor/lib64/soundfx/libv4a_re.so"
    ui_print "  Installed 64-bit Legacy library"
    log "Installed 64-bit Legacy library"
  fi
fi

# ── Configuration File Patching ───────────────────────────────────────────
ui_print "- Patching audio effect configuration files..."

V4A_AIDL_LIB='<library name="v4a_aidl" path="libv4a_aidl.so"/>'
V4A_AIDL_FX='<effect name="v4a_standard_aidl" library="v4a_aidl" uuid="90380da3-8536-4744-a6a3-5731970e640f" type="7261676f-6d75-7369-6364-28e2fd3ac39e"/>'

V4A_LEGACY_LIB='<library name="v4a_re" path="libv4a_re.so"/>'
V4A_LEGACY_FX='<effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>'

patch_target_file() {
  local SRC="$1"
  local REL="${SRC#/}"
  local DEST=""

  case "$REL" in
    vendor/*|odm/*|product/*|system_ext/*|my_product/*|my_stock/*|my_engineering/*|my_preload/*|vivo_product/*)
      DEST="$MODPATH/system/$REL"
      ;;
    system/*)
      DEST="$MODPATH/$REL"
      ;;
    *)
      DEST="$MODPATH/system/$REL"
      ;;
  esac

  mkdir -p "$(dirname "$DEST")"
  cp -f "$SRC" "$DEST"

  case "$DEST" in
    *.conf)
      sed -i "/v4a_standard_re {/,/}/d" "$DEST"
      sed -i "/v4a_standard_aidl {/,/}/d" "$DEST"
      sed -i "/v4a_re {/,/}/d" "$DEST"
      sed -i "/v4a_aidl {/,/}/d" "$DEST"
      if $USE_AIDL; then
        sed -i "s/^[[:space:]]*effects[[:space:]]*{/effects {\n  v4a_standard_aidl {\n    library v4a_aidl\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$DEST"
        sed -i "s/^[[:space:]]*libraries[[:space:]]*{/libraries {\n  v4a_aidl {\n    path \/vendor\/lib\/soundfx\/libv4a_aidl.so\n  }/g" "$DEST"
      else
        sed -i "s/^[[:space:]]*effects[[:space:]]*{/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$DEST"
        sed -i "s/^[[:space:]]*libraries[[:space:]]*{/libraries {\n  v4a_re {\n    path \/vendor\/lib\/soundfx\/libv4a_re.so\n  }/g" "$DEST"
      fi
      ;;
    *.xml)
      sed -i "/v4a_standard_re/d" "$DEST"
      sed -i "/v4a_standard_aidl/d" "$DEST"
      sed -i "/v4a_re/d" "$DEST"
      sed -i "/v4a_aidl/d" "$DEST"
      sed -i 's|<libraries/>|<libraries>\n</libraries>|g' "$DEST"
      sed -i 's|<effects/>|<effects>\n</effects>|g' "$DEST"
      if $USE_AIDL; then
        if grep -q '<libraries>' "$DEST"; then
          sed -i "/<libraries>/ a\\        $V4A_AIDL_LIB" "$DEST"
        elif grep -q '<library ' "$DEST"; then
          sed -i "/<library / i\\        $V4A_AIDL_LIB" "$DEST"
        fi
        if grep -q '<effects>' "$DEST"; then
          sed -i "/<effects>/ a\\        $V4A_AIDL_FX" "$DEST"
        elif grep -q '<effect ' "$DEST"; then
          sed -i "/<effect / i\\        $V4A_AIDL_FX" "$DEST"
        fi
      else
        if grep -q '<libraries>' "$DEST"; then
          sed -i "/<libraries>/ a\\        $V4A_LEGACY_LIB" "$DEST"
        elif grep -q '<library ' "$DEST"; then
          sed -i "/<library / i\\        $V4A_LEGACY_LIB" "$DEST"
        fi
        if grep -q '<effects>' "$DEST"; then
          sed -i "/<effects>/ a\\        $V4A_LEGACY_FX" "$DEST"
        elif grep -q '<effect ' "$DEST"; then
          sed -i "/<effect / i\\        $V4A_LEGACY_FX" "$DEST"
        fi
      fi
      ;;
  esac

  ui_print "  Patched: $SRC"
  log "Patched config: $SRC -> $DEST"
}

PATCHED_COUNT=0
CONFIG_XML_COUNT=0

# Comprehensive OEM audio effects config discovery:
# Recursively searches /odm/etc, /vendor/etc, /system/etc, /product/etc, /system_ext/etc,
# /my_product/etc (ColorOS/OxygenOS), /my_stock/etc, /my_engineering/etc, /my_preload/etc,
# /vivo_product/etc (Vivo/iQOO), and all subdirectories (/etc/audio/**, /etc/audio/sku_*).
find_target_configs() {
  for root_dir in \
    /odm/etc /vendor/etc /system/etc /product/etc /system_ext/etc \
    /my_product/etc /my_stock/etc /my_engineering/etc /my_preload/etc \
    /vivo_product/etc; do
    [ -d "$root_dir" ] || continue
    find "$root_dir" -type f \( \
      -name "audio_effects*.xml" -o \
      -name "audio_effects*.conf" -o \
      -name "*audio_effects_config*.xml" \
    \) 2>/dev/null
  done
}

for cfg in $(find_target_configs); do
  [ -f "$cfg" ] || continue
  patch_target_file "$cfg"
  PATCHED_COUNT=$((PATCHED_COUNT + 1))
  case "$(basename "$cfg")" in
    *audio_effects_config.xml) CONFIG_XML_COUNT=$((CONFIG_XML_COUNT + 1)) ;;
  esac
done

# APEX configs (e.g. Pixel / com.android.hardware.audio.effect)
for apex_dir in /apex/*/; do
  apex_name="$(basename "$apex_dir")"
  apex_cfg="$apex_dir/etc/audio_effects_config.xml"
  [ -f "$apex_cfg" ] || continue
  dest_apex="$MODPATH/apex/$apex_name/etc/audio_effects_config.xml"
  mkdir -p "$(dirname "$dest_apex")"
  cp -f "$apex_cfg" "$dest_apex"
  sed -i "/v4a_standard_aidl/d" "$dest_apex"
  sed -i "/v4a_aidl/d" "$dest_apex"
  sed -i 's|<libraries/>|<libraries>\n</libraries>|g' "$dest_apex"
  sed -i 's|<effects/>|<effects>\n</effects>|g' "$dest_apex"
  sed -i "/<libraries/ a\\        $V4A_AIDL_LIB" "$dest_apex"
  sed -i "/<effects/ a\\        $V4A_AIDL_FX" "$dest_apex"
  ui_print "  Patched APEX config: $apex_cfg"
  log "Patched APEX config: $apex_cfg -> $dest_apex"
  PATCHED_COUNT=$((PATCHED_COUNT + 1))
  CONFIG_XML_COUNT=$((CONFIG_XML_COUNT + 1))
done

# Fallback template if no AIDL config was found on device
if $USE_AIDL && [ "$CONFIG_XML_COUNT" -eq 0 ]; then
  ui_print "  No existing audio_effects_config.xml found, deploying bundled template"
  log "Deploying bundled fallback audio_effects_config.xml"
  mkdir -p "$MODPATH/system/vendor/etc"
  if [ -f "$MODPATH/common/audio_effects_config.xml" ]; then
    cp -f "$MODPATH/common/audio_effects_config.xml" "$MODPATH/system/vendor/etc/audio_effects_config.xml"
  else
    cat << 'EOF' > "$MODPATH/system/vendor/etc/audio_effects_config.xml"
<?xml version="1.0" encoding="UTF-8"?>
<audio_effects_conf version="2.0" xmlns="http://schemas.android.com/audio/audio_effects_conf/v2_0">
    <libraries>
        <library name="v4a_aidl" path="libv4a_aidl.so"/>
    </libraries>
    <effects>
        <effect name="v4a_standard_aidl" library="v4a_aidl" uuid="90380da3-8536-4744-a6a3-5731970e640f" type="7261676f-6d75-7369-6364-28e2fd3ac39e"/>
    </effects>
</audio_effects_conf>
EOF
  fi
fi

log "Total configs patched: $PATCHED_COUNT (audio_effects_config.xml: $CONFIG_XML_COUNT)"

# Partition symlinks for KernelSU / APatch compatibility across AOSP and OEM ROMs
for part in vendor odm product system_ext my_product my_stock my_engineering my_preload vivo_product; do
  if [ -d "$MODPATH/system/$part" ] && [ ! -e "$MODPATH/$part" ]; then
    ln -s "system/$part" "$MODPATH/$part" 2>/dev/null
    log "Created partition symlink: $MODPATH/$part -> system/$part"
  fi
done

# Clean up stale v1 shared memory files and directories
SHM_DIR=/data/local/tmp/v4a
for stale in "$SHM_DIR/shm_hp.bin" "$SHM_DIR/shm_spk.bin"; do
  if [ -f "$stale" ]; then
    rm -f "$stale" && log "Removed stale v1 SHM file: $stale"
  fi
done
for stale_dir in "$SHM_DIR/hp" "$SHM_DIR/spk"; do
  if [ -d "$stale_dir" ]; then
    rm -rf "$stale_dir" && log "Removed stale v1 dir: $stale_dir"
  fi
done

# Clean up common/files from final module installation
rm -rf "$MODPATH/common/files" 2>/dev/null

# ── Permissions & SELinux Contexts ──────────────────────────────────────────
ui_print "- Setting permissions..."
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/system/vendor/lib/soundfx" 0 0 0755 0644
set_perm_recursive "$MODPATH/system/vendor/lib64/soundfx" 0 0 0755 0644
chmod 755 "$MODPATH"/*.sh 2>/dev/null

chcon -R u:object_r:vendor_file:s0 "$MODPATH/system/vendor/lib/soundfx" 2>/dev/null
chcon -R u:object_r:vendor_file:s0 "$MODPATH/system/vendor/lib64/soundfx" 2>/dev/null

find "$MODPATH" -type f \( -name "audio_effects*.xml" -o -name "audio_effects*.conf" \) 2>/dev/null | while read f; do
  chcon u:object_r:vendor_configs_file:s0 "$f" 2>/dev/null
done

ui_print " "
ui_print "- Installation complete!"
ui_print "- Please reboot your device to activate."
ui_print "- Detailed install log: $V4A_LOG"

log "---> ViPER4Android Installation Finished Successfully <---"