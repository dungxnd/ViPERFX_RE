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

# ── HAL Type Detection: Symmetric AIDL vs Legacy (HIDL) Probing ─────────────
ui_print "- Detecting audio HAL type..."

vintf_hal_format() {
  local name="$1" fmt="$2"
  local files
  files="$(grep -rl "$name" /vendor/etc/vintf/ /vendor/manifest.xml /odm/etc/vintf/ 2>/dev/null)"
  [ -z "$files" ] && return 1
  for f in $files; do
    if awk -v name="$name" -v fmt="$fmt" '
      /<hal/{in_hal=1; has_fmt=0; has_name=0}
      in_hal && $0 ~ ("format=\"" fmt "\""){has_fmt=1}
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

AIDL_SCORE=0
HIDL_SCORE=0

# Check manual overrides first
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

# AIDL Probes
if lshal 2>/dev/null | grep -qF "android.hardware.audio.effect.IFactory"; then
  ui_print "    [AIDL A1] lshal: AIDL IFactory vendor HAL confirmed"
  AIDL_SCORE=$((AIDL_SCORE + 10))
fi

if ls /vendor/bin/hw/*audio*aidl* 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/*audio*mediatek* 1>/dev/null 2>&1 || \
   ls /vendor/bin/hw/secaudiohalaidl 1>/dev/null 2>&1 || \
   ls /apex/com.android.hardware.audio/bin/hw/*audio* 1>/dev/null 2>&1; then
  ui_print "    [AIDL A2] AIDL HAL binary found in vendor or APEX"
  AIDL_SCORE=$((AIDL_SCORE + 5))
fi

if vintf_hal_format "android.hardware.audio.effect" "aidl"; then
  ui_print "    [AIDL A3] Vendor VINTF declares AIDL audio effect HAL"
  AIDL_SCORE=$((AIDL_SCORE + 5))
fi

if [ -f "/vendor/etc/audio_effects_config.xml" ] && \
   ! [ -f "/vendor/etc/audio_effects.conf" ] && \
   ! [ -f "/vendor/etc/audio_effects.xml" ]; then
  ui_print "    [AIDL A4] Standalone audio_effects_config.xml found"
  AIDL_SCORE=$((AIDL_SCORE + 3))
fi

if [ "$(getprop init.svc.vendor.audio-hal-aidl 2>/dev/null)" = "running" ] || \
   [ "$(getprop init.svc.vendor.audio-effect-hal-aidl 2>/dev/null)" = "running" ] || \
   [ "$(getprop init.svc.audio-hal-aidl 2>/dev/null)" = "running" ]; then
  ui_print "    [AIDL A5] AIDL HAL service property running"
  AIDL_SCORE=$((AIDL_SCORE + 4))
elif [ "$API" -ge 33 ] && ps -A 2>/dev/null | grep -qE '([Aa]udio.*[Aa]idl|[Aa]idl.*[Aa]udio|secaudiohalaidl)'; then
  ui_print "    [AIDL A5] AIDL audio daemon process found in ps"
  AIDL_SCORE=$((AIDL_SCORE + 3))
fi

# HIDL Probes
if lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect@[0-9]+\.[0-9]+::IEffectsFactory"; then
  ui_print "    [HIDL H1] lshal: HIDL IEffectsFactory vendor HAL confirmed"
  HIDL_SCORE=$((HIDL_SCORE + 10))
fi

if ls /vendor/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1 || \
   ls /system/lib*/soundfx/libeffectproxy.so 1>/dev/null 2>&1; then
  ui_print "    [HIDL H2] libeffectproxy.so found (HIDL effects proxy)"
  HIDL_SCORE=$((HIDL_SCORE + 4))
fi

if ls /vendor/etc/audio_effects.conf 1>/dev/null 2>&1 || \
   ls /vendor/etc/audio_effects.xml 1>/dev/null 2>&1; then
  ui_print "    [HIDL H3] Legacy audio_effects config found"
  HIDL_SCORE=$((HIDL_SCORE + 3))
fi

if vintf_hal_format "android.hardware.audio.effect" "hidl"; then
  ui_print "    [HIDL H4] Vendor VINTF declares HIDL audio effect HAL"
  HIDL_SCORE=$((HIDL_SCORE + 4))
fi

if [ "$HIDL_SCORE" -eq 0 ] && lshal 2>/dev/null | grep -qE "android\.hardware\.audio\.effect@[0-9]"; then
  ui_print "    [HIDL H5] lshal: HIDL audio effect entry found"
  HIDL_SCORE=$((HIDL_SCORE + 2))
fi

ui_print "    Detection scores: AIDL=$AIDL_SCORE, HIDL=$HIDL_SCORE (API $API)"
log "Scores: AIDL=$AIDL_SCORE, HIDL=$HIDL_SCORE, API=$API"

USE_AIDL=false
if [ -n "$OVERRIDE_MODE" ]; then
  [ "$OVERRIDE_MODE" = "aidl" ] && USE_AIDL=true || USE_AIDL=false
elif [ "$AIDL_SCORE" -gt 0 ] && [ "$AIDL_SCORE" -ge "$HIDL_SCORE" ]; then
  USE_AIDL=true
elif [ "$API" -ge 35 ] && [ "$HIDL_SCORE" -eq 0 ]; then
  ui_print "    Android 15+ (API $API) defaults to AIDL HAL"
  USE_AIDL=true
elif [ "$HIDL_SCORE" -gt 0 ]; then
  USE_AIDL=false
elif [ "$API" -ge 35 ]; then
  ui_print "    Android 15+ (API $API): preferring AIDL"
  USE_AIDL=true
else
  ui_print "    ! No definitive HAL evidence found; defaulting to Legacy"
  USE_AIDL=false
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
    vendor/*|odm/*|product/*|system_ext/*)
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

CONFIG_DIRS="/odm/etc /vendor/etc /system/etc /product/etc /system_ext/etc"
for d in $CONFIG_DIRS; do
  [ -d "$d" ] || continue
  for cfg in "$d"/audio_effects*.xml "$d"/audio_effects*.conf; do
    [ -f "$cfg" ] || continue
    patch_target_file "$cfg"
    PATCHED_COUNT=$((PATCHED_COUNT + 1))
    case "$(basename "$cfg")" in
      *audio_effects_config.xml) CONFIG_XML_COUNT=$((CONFIG_XML_COUNT + 1)) ;;
    esac
  done
done

for sku_dir in /vendor/etc/audio/sku_* /odm/etc/audio/sku_*; do
  [ -d "$sku_dir" ] || continue
  for cfg in "$sku_dir"/audio_effects*.xml "$sku_dir"/audio_effects*.conf; do
    [ -f "$cfg" ] || continue
    patch_target_file "$cfg"
    PATCHED_COUNT=$((PATCHED_COUNT + 1))
    case "$(basename "$cfg")" in
      *audio_effects_config.xml) CONFIG_XML_COUNT=$((CONFIG_XML_COUNT + 1)) ;;
    esac
  done
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

# Partition symlinks for KernelSU / APatch compatibility
for part in vendor odm product system_ext; do
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