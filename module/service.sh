#!/system/bin/sh
MODDIR=${0%/*}

V4A_LOG=/data/adb/viper_install.log
log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [service] $1" >> "$V4A_LOG"; }

log "---> service.sh started <---"

# Wait for boot completion (up to 40 seconds)
counter=0
until [ "$(getprop sys.boot_completed)" = "1" ] || [ $counter -ge 40 ]; do
  sleep 1
  counter=$((counter + 1))
done
log "Boot completed reached (waited ${counter}s)"

# Determine driver mode
USE_AIDL=true
if [ -f "$MODDIR/aidl_mode.txt" ]; then
  [ "$(cat "$MODDIR/aidl_mode.txt" 2>/dev/null)" = "legacy" ] && USE_AIDL=false
fi

if $USE_AIDL; then
  EFFECT_LIB="libv4a_aidl.so"
  EFFECT_MATCH="v4a_aidl"
  V4A_LIB='<library name="v4a_aidl" path="libv4a_aidl.so"/>'
  V4A_FX='<effect name="v4a_standard_aidl" library="v4a_aidl" uuid="90380da3-8536-4744-a6a3-5731970e640f" type="7261676f-6d75-7369-6364-28e2fd3ac39e"/>'
  log "Mode: AIDL ($EFFECT_LIB)"
else
  EFFECT_LIB="libv4a_re.so"
  EFFECT_MATCH="v4a_re"
  V4A_LIB='<library name="v4a_re" path="libv4a_re.so"/>'
  V4A_FX='<effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>'
  log "Mode: Legacy ($EFFECT_LIB)"
fi

MOD_VENDOR="$MODDIR/system/vendor"

set_prop_safe() {
  local prop="$1"
  local val="$2"
  if command -v resetprop >/dev/null 2>&1; then
    resetprop -n "$prop" "$val" 2>/dev/null
  elif [ -x /data/adb/magisk/resetprop ]; then
    /data/adb/magisk/resetprop -n "$prop" "$val" 2>/dev/null
  elif [ -x /data/adb/ksu/bin/resetprop ]; then
    /data/adb/ksu/bin/resetprop -n "$prop" "$val" 2>/dev/null
  elif [ -x /data/adb/ap/bin/resetprop ]; then
    /data/adb/ap/bin/resetprop -n "$prop" "$val" 2>/dev/null
  else
    setprop "$prop" "$val" 2>/dev/null
  fi
}

ns_mount() {
  # Enter init's mount namespace (PID 1) so daemons respawned by init inherit this mount
  nsenter -t 1 -m -- mount -o bind "$1" "$2" 2>/dev/null
  # Also mount in current namespace
  mount -o bind "$1" "$2" 2>/dev/null
  return 0
}

MOUNTED=false

# ── 1. Bind-Mount Effect Libraries ───────────────────────────────────────────
mount_effect_lib() {
  local SYS_TARGET="$1"
  local MOD_SOURCE_DIR="$2"
  local WORK_DIR="$MODDIR/work_mount$(echo "$SYS_TARGET" | tr '/' '_')"

  [ -d "$SYS_TARGET" ] || return
  [ -f "$MOD_SOURCE_DIR/$EFFECT_LIB" ] || return

  # Check if lib already exists and is readable in target directory
  if [ -f "$SYS_TARGET/$EFFECT_LIB" ]; then
    log "mount_effect_lib: $SYS_TARGET already contains $EFFECT_LIB"
    return
  fi

  rm -rf "$WORK_DIR" 2>/dev/null
  mkdir -p "$WORK_DIR"

  cp -af "$SYS_TARGET"/* "$WORK_DIR/" 2>/dev/null
  cp -f "$MOD_SOURCE_DIR/$EFFECT_LIB" "$WORK_DIR/$EFFECT_LIB"
  chcon -R --reference="$SYS_TARGET" "$WORK_DIR" 2>/dev/null
  ns_mount "$WORK_DIR" "$SYS_TARGET"
  log "mount_effect_lib: bind-mounted shadow $WORK_DIR -> $SYS_TARGET"
  MOUNTED=true
}

# Vendor, ODM, System, System_ext, and OEM libraries
for arch in lib lib64; do
  for sfx_path in \
    "/vendor/$arch/soundfx" \
    "/system/vendor/$arch/soundfx" \
    "/odm/$arch/soundfx" \
    "/system/odm/$arch/soundfx" \
    "/system_ext/$arch/soundfx" \
    "/system/$arch/soundfx" \
    "/my_product/$arch/soundfx"; do
    mount_effect_lib "$sfx_path" "$MOD_VENDOR/$arch/soundfx"
  done
done

# APEX soundfx libraries (Pixel / com.android.hardware.audio.effect linker namespace)
for sfx_dir in /apex/*/lib64/soundfx /apex/*/lib/soundfx; do
  [ -d "$sfx_dir" ] || continue
  case "$sfx_dir" in
    */lib64/*) arch="lib64" ;;
    *)         arch="lib" ;;
  esac
  mount_effect_lib "$sfx_dir" "$MOD_VENDOR/$arch/soundfx"
done

# ── 2. Bind-Mount Audio Effect Configurations ────────────────────────────────
mount_config() {
  local original="$1"
  local patched=""

  # Check if PID 1 already sees v4a entry in this config
  local has_v4a=""
  has_v4a=$(nsenter -t 1 -m -- grep -c "$EFFECT_MATCH" "$original" 2>/dev/null)
  if [ -z "$has_v4a" ]; then
    has_v4a=$(grep -c "$EFFECT_MATCH" "$original" 2>/dev/null)
  fi
  if [ "${has_v4a:-0}" -gt 0 ]; then
    return
  fi

  # Find pre-patched file in module directory
  # Priority 1: Exact relative path match
  local rel="${original#/}"
  for cand in "$MODDIR/$rel" "$MODDIR/system/$rel"; do
    if [ -f "$cand" ] && grep -q "$EFFECT_MATCH" "$cand" 2>/dev/null; then
      patched="$cand"
      break
    fi
  done

  # Priority 2: Fallback search by filename
  if [ -z "$patched" ]; then
    for p in $(find "$MODDIR/system" "$MODDIR/apex" -type f -name "$(basename "$original")" 2>/dev/null); do
      if grep -q "$EFFECT_MATCH" "$p" 2>/dev/null; then
        patched="$p"
        break
      fi
    done
  fi

  # On-the-fly patching if no pre-patched match
  if [ -z "$patched" ]; then
    patched="$MODDIR/work_cfg/$(echo "$original" | tr '/' '_')"
    mkdir -p "$MODDIR/work_cfg"
    cp -f "$original" "$patched"
    case "$original" in
      *.xml)
        sed -i "/v4a_standard_re/d" "$patched"
        sed -i "/v4a_standard_aidl/d" "$patched"
        sed -i "/v4a_re/d" "$patched"
        sed -i "/v4a_aidl/d" "$patched"
        sed -i 's|<libraries/>|<libraries>\n</libraries>|g' "$patched"
        sed -i 's|<effects/>|<effects>\n</effects>|g' "$patched"
        if grep -q '<libraries>' "$patched"; then
          sed -i "/<libraries>/ a\\        $V4A_LIB" "$patched"
        elif grep -q '<library ' "$patched"; then
          sed -i "/<library / i\\        $V4A_LIB" "$patched"
        fi
        if grep -q '<effects>' "$patched"; then
          sed -i "/<effects>/ a\\        $V4A_FX" "$patched"
        elif grep -q '<effect ' "$patched"; then
          sed -i "/<effect / i\\        $V4A_FX" "$patched"
        fi
        ;;
      *.conf)
        if [ "$USE_AIDL" = false ]; then
          sed -i "/v4a_re {/,/}/d" "$patched"
          sed -i "/v4a_standard_re {/,/}/d" "$patched"
          if grep -q 'libraries {' "$patched"; then
            sed -i "/libraries {/ a\\  v4a_re {\\n    path \\/vendor\\/lib\\/soundfx\\/libv4a_re.so\\n  }" "$patched"
          fi
          if grep -q 'effects {' "$patched"; then
            sed -i "/effects {/ a\\  v4a_standard_re {\\n    library v4a_re\\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\\n  }" "$patched"
          fi
        fi
        ;;
    esac
    chcon --reference="$original" "$patched" 2>/dev/null
    log "Config patched on-the-fly: $original"
  fi

  if cmp -s "$patched" "$original" 2>/dev/null; then
    return
  fi

  ns_mount "$patched" "$original"
  log "Config mounted: $patched -> $original"
  MOUNTED=true
}

# Comprehensive OEM live config discovery (Xiaomi, Samsung, ColorOS, Vivo, MediaTek, QTI)
find_live_configs() {
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

for cfg in $(find_live_configs); do
  [ -f "$cfg" ] || continue
  mount_config "$cfg"
done

for apex_dir in /apex/*/; do
  apex_cfg="$apex_dir/etc/audio_effects_config.xml"
  [ -f "$apex_cfg" ] || continue
  mount_config "$apex_cfg"
done

# ── 3. Restart Audio HAL Processes ───────────────────────────────────────────
# If any configs or libraries were mounted, kill running audio HAL daemons so init
# reloads them cleanly with the newly active effect definitions.
if [ "$MOUNTED" = true ]; then
  log "Restarting audio HAL daemons to apply configuration..."
  KNOWN_DAEMONS="
    audioserver
    secaudiohalaidl
    audiohalservice
    audio.service-aidl.mediatek
    vendor.audio-hal-aidl
    vendor.qti.hardware.audio.service-aidl
    vendor.oplus.hardware.audio.service
    vendor.vivo.hardware.audio.service
    vendor.mediatek.hardware.audio.service
    android.hardware.audio.service
    android.hardware.audio.effect.service
  "
  for proc in $KNOWN_DAEMONS; do
    pids=$(ps -A 2>/dev/null | grep "$proc" | grep -v grep | awk '{print $2}')
    if [ -n "$pids" ]; then
      for pid in $pids; do
        kill -9 "$pid" 2>/dev/null
      done
      log "Terminated process $proc (PIDs: $pids)"
    fi
  done

  # Catch-all for generic OEM AIDL / audio HAL daemon processes
  ps -A 2>/dev/null | grep -iE '([Aa]udio.*[Aa]idl|[Aa]idl.*[Aa]udio|hardware\.audio\.service|secaudiohalaidl)' | grep -v grep | awk '{print $2}' | while read -r pid; do
    kill -9 "$pid" 2>/dev/null
  done
  sleep 2
fi

# ── 4. Background Property Guardian ──────────────────────────────────────────
# Ensure audio effect inhibition props remain disabled across Samsung, OPlus, Qualcomm, etc.
(
  c=0
  while [ $c -le 30 ]; do
    if [ "$(getprop ro.audio.ignore_effects 2>/dev/null)" = "true" ]; then
      set_prop_safe ro.audio.ignore_effects false
      log "Reset ro.audio.ignore_effects to false (iteration $c)"
    fi
    if [ "$(getprop ro.vendor.audio.ignore_effects 2>/dev/null)" = "true" ]; then
      set_prop_safe ro.vendor.audio.ignore_effects false
      log "Reset ro.vendor.audio.ignore_effects to false (iteration $c)"
    fi
    for p in vendor.audio.effects.enable persist.vendor.audio.effects.enable; do
      val=$(getprop "$p" 2>/dev/null)
      if [ "$val" = "0" ] || [ "$val" = "false" ]; then
        set_prop_safe "$p" true
        log "Enforced $p to true (iteration $c)"
      fi
    done
    sleep 2
    c=$((c + 1))
  done
) >/dev/null 2>&1 &

# ── 5. Shared Memory Permissions Verification ────────────────────────────────
mkdir -p /data/local/tmp/v4a
chmod 777 /data/local/tmp/v4a 2>/dev/null
chmod 666 /data/local/tmp/v4a/*.bin 2>/dev/null
chcon -R u:object_r:shell_data_file:s0 /data/local/tmp/v4a 2>/dev/null

log "---> service.sh finished successfully <---"