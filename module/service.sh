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

ns_mount() {
  if nsenter -t 1 -m -- mount -o bind "$1" "$2" 2>/dev/null; then
    return 0
  fi
  mount -o bind "$1" "$2"
}

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
  log "mount_effect_lib: bind-mounted shadow $WORK_DIR -> $SYS_TARGET (rc=$?)"
}

# Vendor libraries
for arch in lib lib64; do
  mount_effect_lib "/vendor/$arch/soundfx" "$MOD_VENDOR/$arch/soundfx"
  mount_effect_lib "/system/vendor/$arch/soundfx" "$MOD_VENDOR/$arch/soundfx"
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
MOUNTED=false

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
  for p in $(find "$MODDIR/system" "$MODDIR/apex" -type f -name "$(basename "$original")" 2>/dev/null); do
    if grep -q "$EFFECT_MATCH" "$p" 2>/dev/null; then
      patched="$p"
      break
    fi
  done

  # On-the-fly patching if no pre-patched match
  if [ -z "$patched" ]; then
    patched="$MODDIR/work_cfg/$(echo "$original" | tr '/' '_')"
    mkdir -p "$MODDIR/work_cfg"
    cp -f "$original" "$patched"
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
    chcon --reference="$original" "$patched" 2>/dev/null
    log "Config patched on-the-fly: $original"
  fi

  if cmp -s "$patched" "$original" 2>/dev/null; then
    return
  fi

  ns_mount "$patched" "$original"
  log "Config mounted: $patched -> $original (rc=$?)"
  MOUNTED=true
}

CONFIG_DIRS="/odm/etc /vendor/etc /system/etc /product/etc /system_ext/etc"
for d in $CONFIG_DIRS; do
  [ -d "$d" ] || continue
  for cfg in "$d"/audio_effects*.xml; do
    [ -f "$cfg" ] || continue
    mount_config "$cfg"
  done
done

for sku_dir in /vendor/etc/audio/sku_* /odm/etc/audio/sku_*; do
  [ -d "$sku_dir" ] || continue
  for cfg in "$sku_dir"/audio_effects*.xml; do
    [ -f "$cfg" ] || continue
    mount_config "$cfg"
  done
done

for apex_dir in /apex/*/; do
  apex_cfg="$apex_dir/etc/audio_effects_config.xml"
  [ -f "$apex_cfg" ] || continue
  mount_config "$apex_cfg"
done

# ── 3. Restart Audio HAL Processes ───────────────────────────────────────────
# If any configs were newly mounted, kill running audio HAL daemons so init
# reloads them cleanly with the newly active effect definitions.
if [ "$MOUNTED" = true ]; then
  log "Restarting audio HAL daemons to apply configuration..."
  for proc in audioserver secaudiohalaidl audiohalservice audio.service-aidl.mediatek; do
    pids=$(ps -A 2>/dev/null | grep "$proc" | grep -v grep | awk '{print $2}')
    if [ -n "$pids" ]; then
      for pid in $pids; do
        kill -9 "$pid" 2>/dev/null
      done
      log "Terminated process $proc (PIDs: $pids)"
    fi
  done

  # Catch-all for generic OEM AIDL audio HAL processes
  ps -A 2>/dev/null | grep -E '([Aa]udio.*[Aa]idl|[Aa]idl.*[Aa]udio)' | grep -v grep | awk '{print $2}' | while read -r pid; do
    kill -9 "$pid" 2>/dev/null
  done
  sleep 2
fi

# ── 4. Background Property Guardian ──────────────────────────────────────────
# Ensure ro.audio.ignore_effects remains disabled
(
  c=0
  while [ $c -le 30 ]; do
    if [ "$(getprop ro.audio.ignore_effects)" = "true" ]; then
      resetprop -n ro.audio.ignore_effects false 2>/dev/null
      log "Reset ro.audio.ignore_effects to false (iteration $c)"
    fi
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