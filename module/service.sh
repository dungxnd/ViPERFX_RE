#!/system/bin/sh
# service.sh — runs in Magisk/KSU/APatch "late_start" service mode.
# This hook fires AFTER apexd has mounted all APEX containers (unlike
# post-fs-data.sh which fires before apexd).  Use it for work that requires
# /apex/* to be fully populated.
#
# Current responsibility:
#   Retry the APEX audio_effects_config.xml bind-mounts that post-fs-data.sh
#   may have skipped because the APEX was not yet mounted at that early stage.
#   This is the primary path for devices where the audio effect HAL runs inside
#   an APEX (e.g. Pixel 8/9, AOSP com.android.hardware.audio.effect).

MODDIR=${0%/*}

# Wait for boot to complete so audioserver has not yet opened its first session.
# On most devices APEXes are mounted well before boot_completed, but the wait
# ensures we do not race against a very slow apexd start.
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done

# ── APEX config bind-mount (late retry) ───────────────────────────────────────
# post-fs-data.sh already attempts these mounts.  This block re-runs the same
# idempotent logic for devices where post-fs-data ran before apexd finished.
APEX_BASE="$MODDIR/apex"
if [ -d "$APEX_BASE" ]; then
  for STORED_CFG in "$APEX_BASE"/*/etc/audio_effects_config.xml; do
    [ -f "$STORED_CFG" ] || continue

    APEX_NAME="$(echo "$STORED_CFG" | sed "s|$APEX_BASE/||;s|/etc/audio_effects_config.xml||")"
    LIVE_CFG="/apex/$APEX_NAME/etc/audio_effects_config.xml"

    # Skip if the APEX is not mounted or the target config does not exist.
    [ -f "$LIVE_CFG" ] || continue

    # Idempotent: skip if already bind-mounted (handles post-fs-data success path).
    if grep -q "^$STORED_CFG $LIVE_CFG" /proc/mounts 2>/dev/null || \
       grep -q " $LIVE_CFG " /proc/mounts 2>/dev/null; then
      log -t ViPER4Android "APEX config already mounted, skipping: $LIVE_CFG"
      continue
    fi

    mount -o bind "$STORED_CFG" "$LIVE_CFG" 2>/dev/null && \
      log -t ViPER4Android "APEX config bind-mounted via service.sh: $LIVE_CFG" || \
      log -t ViPER4Android "APEX config bind-mount FAILED via service.sh: $LIVE_CFG"
  done
fi

# ── /data/local/tmp/v4a directory and file permissions ────────────────────────
# The SHM files are created by ConfigChannel.kt (app side) and written by
# the AIDL driver (libv4a_aidl.so).  SELinux context + world-readable
# permissions must be set so both the app process and the HAL process can map them.
mkdir -p /data/local/tmp/v4a
chmod 777 /data/local/tmp/v4a
# Fix permissions on any SHM files already created (e.g. by a previous boot).
chmod 666 /data/local/tmp/v4a/*.bin 2>/dev/null
chcon -R u:object_r:shell_data_file:s0 /data/local/tmp/v4a 2>/dev/null
