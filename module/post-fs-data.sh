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

# install.sh uses place_file() which routes ODM config to $MODPATH/system/odm/etc/.
# Check both possible locations so this works regardless of root manager layout.
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
