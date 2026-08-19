#!/system/bin/sh
MODDIR=${0%/*}

# post-fs-data.sh — runtime phase (partitions are still RO at this point).
# DO NOT run sed -i against /system /vendor /odm — they are mounted read-only.
# All patched config files were written into $MODPATH during installation by
# install.sh using place_file(), so the root manager's overlay (Magisk magic
# mount / KernelSU + APatch OverlayFS) handles them automatically.
#
# The only safe operation here is a bind-mount fallback for ODM when the root
# manager has not created an overlay entry for it (rare, device-specific).

if [ -d "/odm/etc" ] && [ -f "$MODDIR/odm/etc/audio_effects.xml" ]; then
  # Only bind-mount if not already overlaid
  if ! grep -q "^overlay /odm" /proc/mounts 2>/dev/null; then
    mount -o bind "$MODDIR/odm/etc/audio_effects.xml" /odm/etc/audio_effects.xml 2>/dev/null
  fi
fi
