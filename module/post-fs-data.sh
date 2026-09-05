#!/system/bin/sh
MODDIR=${0%/*}

V4A_LOG=/data/adb/viper_install.log
log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [post-fs-data] $1" >> "$V4A_LOG"; }

log "---> post-fs-data started <---"
log "SELinux: $(getenforce 2>/dev/null || echo unknown)"

# ── 1. Shared Memory Pre-creation & Permissions ──────────────────────────────
# Audio HAL runs under a restricted SELinux domain (hal_audio_default, mtk_hal_audio).
# Creating these files early with world read-write permissions and shell_data_file context
# ensures the driver can map and update them without needing directory-creation rights.
SHM_DIR=/data/local/tmp/v4a
SHM_STATUS=$SHM_DIR/shm_status.bin
SHM_PARAMS=$SHM_DIR/shm_params.bin
SHM_BULK=$SHM_DIR/shm_bulk.bin
KERNEL_DIR=$SHM_DIR/kernel

mkdir -p "$SHM_DIR"
chmod 777 "$SHM_DIR"
chcon u:object_r:shell_data_file:s0 "$SHM_DIR" 2>/dev/null

[ -f "$SHM_STATUS" ] || dd if=/dev/zero of="$SHM_STATUS" bs=256 count=1 2>/dev/null
[ -f "$SHM_PARAMS" ] || dd if=/dev/zero of="$SHM_PARAMS" bs=4096 count=1 2>/dev/null
[ -f "$SHM_BULK" ]   || dd if=/dev/zero of="$SHM_BULK"   bs=4096 count=1 2>/dev/null

mkdir -p "$KERNEL_DIR"
chmod 777 "$KERNEL_DIR"
chcon u:object_r:shell_data_file:s0 "$KERNEL_DIR" 2>/dev/null

chmod 666 "$SHM_STATUS" "$SHM_PARAMS" "$SHM_BULK" 2>/dev/null
chcon u:object_r:shell_data_file:s0 "$SHM_STATUS" "$SHM_PARAMS" "$SHM_BULK" 2>/dev/null

log "SHM channel initialized in $SHM_DIR"

# ── 2. Early Property Tweaks ────────────────────────────────────────────────
if command -v resetprop >/dev/null 2>&1; then
  resetprop -n ro.audio.ignore_effects false 2>/dev/null
  log "Early resetprop ro.audio.ignore_effects false applied"
fi

# ── 3. Live SELinux Policy Injection ─────────────────────────────────────────
V4A_POLICY="
allow hal_audio_default shell_data_file dir { search read open getattr write add_name remove_name rename }
allow hal_audio_default shell_data_file file { read write create open getattr setattr unlink rename map }
allow mtk_hal_audio shell_data_file dir { search read open getattr write add_name remove_name rename }
allow mtk_hal_audio shell_data_file file { read write create open getattr setattr unlink rename map }
allow hal_audio_server shell_data_file dir { search read open getattr write add_name remove_name rename }
allow hal_audio_server shell_data_file file { read write create open getattr setattr unlink rename map }
allow audioserver shell_data_file dir { search read open getattr write add_name remove_name rename }
allow audioserver shell_data_file file { read write create open getattr setattr unlink rename map }
allow untrusted_app shell_data_file dir { search read open getattr }
allow untrusted_app shell_data_file file { read write open getattr map }
allow untrusted_app_all shell_data_file dir { search read open getattr }
allow untrusted_app_all shell_data_file file { read write open getattr map }
allow appdomain shell_data_file dir { search read open getattr }
allow appdomain shell_data_file file { read write open getattr map }
allow hal_audio_default vendor_file file { read getattr open execute execute_no_trans map }
allow mtk_hal_audio vendor_file file { read getattr open execute execute_no_trans map }
allow audioserver vendor_file file { read getattr open execute execute_no_trans map }
allow hal_audio_default hal_audio_default process { execmem }
allow mtk_hal_audio mtk_hal_audio process { execmem }
allow audioserver self:process { execmem }
"

POLICY_TOOL=""
POLICY_TYPE=""
if command -v magiskpolicy >/dev/null 2>&1; then
  POLICY_TOOL="magiskpolicy"
  POLICY_TYPE="magisk"
elif [ -x /data/adb/magisk/magiskpolicy ]; then
  POLICY_TOOL="/data/adb/magisk/magiskpolicy"
  POLICY_TYPE="magisk"
elif command -v ksud >/dev/null 2>&1; then
  POLICY_TOOL="ksud"
  POLICY_TYPE="ksu"
elif [ -x /data/adb/ksu/bin/ksud ]; then
  POLICY_TOOL="/data/adb/ksu/bin/ksud"
  POLICY_TYPE="ksu"
elif [ -x /data/adb/ap/bin/apd ]; then
  POLICY_TOOL="/data/adb/ap/bin/apd"
  POLICY_TYPE="apatch"
fi

if [ -n "$POLICY_TOOL" ]; then
  RULE_FILE="$MODDIR/v4a_sepolicy.tmp"
  case "$POLICY_TYPE" in
    ksu)
      echo "$V4A_POLICY" > "$RULE_FILE"
      $POLICY_TOOL sepolicy apply "$RULE_FILE" >/dev/null 2>&1
      rm -f "$RULE_FILE"
      log "SELinux live policy applied via ksud"
      ;;
    apatch)
      echo "$V4A_POLICY" > "$RULE_FILE"
      $POLICY_TOOL sepolicy --live --apply "$RULE_FILE" >/dev/null 2>&1
      rm -f "$RULE_FILE"
      log "SELinux live policy applied via apd"
      ;;
    magisk)
      echo "$V4A_POLICY" | while read -r line; do
        [ -z "$line" ] && continue
        $POLICY_TOOL --live "$line" >/dev/null 2>&1
      done
      log "SELinux live policy applied via magiskpolicy"
      ;;
  esac
else
  log "Notice: No dynamic policy tool found; relying on sepolicy.rule static rules"
fi

# ── 4. ODM Bind-Mount Fallback ───────────────────────────────────────────────
for odm_cand in "$MODDIR/system/odm/etc" "$MODDIR/odm/etc"; do
  [ -d "$odm_cand" ] || continue
  for cfg in "$odm_cand"/audio_effects* "$odm_cand"/audio/sku_*/audio_effects*; do
    [ -f "$cfg" ] || continue
    rel="${cfg#$odm_cand/}"
    live_target="/odm/etc/$rel"
    [ -f "$live_target" ] || continue
    if ! grep -q " $live_target " /proc/mounts 2>/dev/null; then
      mount -o bind "$cfg" "$live_target" 2>/dev/null && \
        log "ODM bind-mount fallback succeeded: $live_target"
    fi
  done
done

# ── 5. APEX Config Early Bind-Mount ──────────────────────────────────────────
APEX_BASE="$MODDIR/apex"
if [ -d "$APEX_BASE" ]; then
  for stored_cfg in "$APEX_BASE"/*/etc/audio_effects_config.xml; do
    [ -f "$stored_cfg" ] || continue
    apex_name="$(echo "$stored_cfg" | sed "s|$APEX_BASE/||;s|/etc/audio_effects_config.xml||")"
    live_cfg="/apex/$apex_name/etc/audio_effects_config.xml"
    [ -f "$live_cfg" ] || continue
    if ! grep -q " $live_cfg " /proc/mounts 2>/dev/null; then
      mount -o bind "$stored_cfg" "$live_cfg" 2>/dev/null && \
        log "APEX config early bind-mount succeeded: $live_cfg"
    fi
  done
fi

log "---> post-fs-data completed <---"