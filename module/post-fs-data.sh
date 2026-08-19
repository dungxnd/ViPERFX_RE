#!/system/bin/sh
MODDIR=${0%/*}

# post-fs-data.sh — runtime phase (partitions are still RO at this point).
# DO NOT run sed -i against /system /vendor /odm — they are mounted read-only.
# All patched config files were written into $MODPATH during installation by
# install.sh using place_file(), so the root manager's overlay (Magisk magic
# mount / KernelSU + APatch OverlayFS) handles vendor/system/odm automatically.
#
# This script handles TWO cases that the overlay system cannot cover:
#
# 1. ODM bind-mount fallback — for root managers that do not create an overlay
#    for the /odm partition (rare, device-specific).
#
# 2. APEX config bind-mount — IFactory running inside an APEX reads its config
#    from /apex/<name>/etc/audio_effects_config.xml, which is a squashfs image
#    and cannot be overlaid by Magisk/KSU/APatch at install time.
#    install.sh (AIDL branch) stores patched copies under:
#      $MODPATH/apex/<name>/etc/audio_effects_config.xml
#    We bind-mount each one over the live APEX path here, before audioserver
#    starts in the boot sequence.
#    EffectConfig::resolveLibrary() still falls through to /vendor/lib64/soundfx/
#    for the .so, so no APEX lib injection is needed.

# ── 1. ODM bind-mount fallback ────────────────────────────────────────────────
ODM_XML=""
if [ -f "$MODDIR/system/odm/etc/audio_effects.xml" ]; then
  ODM_XML="$MODDIR/system/odm/etc/audio_effects.xml"
elif [ -f "$MODDIR/odm/etc/audio_effects.xml" ]; then
  ODM_XML="$MODDIR/odm/etc/audio_effects.xml"
fi

if [ -n "$ODM_XML" ] && [ -d "/odm/etc" ]; then
  # Only bind-mount if /odm is not already overlaid by the root manager
  if ! grep -q "^overlay /odm" /proc/mounts 2>/dev/null; then
    mount -o bind "$ODM_XML" /odm/etc/audio_effects.xml 2>/dev/null
  fi
fi

# ── 2. APEX config bind-mount ─────────────────────────────────────────────────
APEX_BASE="$MODDIR/apex"
if [ -d "$APEX_BASE" ]; then
  for STORED_CFG in "$APEX_BASE"/*/etc/audio_effects_config.xml; do
    [ -f "$STORED_CFG" ] || continue

    # Derive the APEX name from the stored path:
    #   $MODDIR/apex/<name>/etc/audio_effects_config.xml
    APEX_NAME="$(echo "$STORED_CFG" | sed "s|$APEX_BASE/||;s|/etc/audio_effects_config.xml||")"
    LIVE_CFG="/apex/$APEX_NAME/etc/audio_effects_config.xml"

    # Only bind-mount if the APEX is actually mounted and has this config file
    [ -f "$LIVE_CFG" ] || continue

    # Skip if already bind-mounted (idempotent across reboots)
    if grep -q "^$STORED_CFG $LIVE_CFG" /proc/mounts 2>/dev/null || \
       grep -q " $LIVE_CFG " /proc/mounts 2>/dev/null; then
      continue
    fi

    mount -o bind "$STORED_CFG" "$LIVE_CFG" 2>/dev/null && \
      log -t ViPER4Android "APEX config bind-mounted: $LIVE_CFG" || \
      log -t ViPER4Android "APEX config bind-mount FAILED: $LIVE_CFG"
  done
fi
