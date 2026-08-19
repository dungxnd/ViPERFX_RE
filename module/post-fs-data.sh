LIBPATCH=`cat $MODPATH/libpatch.txt`
MODE=`cat $MODPATH/aidl_mode.txt 2>/dev/null`

# ── XML patching ──────────────────────────────────────────────────────────────
# The module overlay copies under $MODPATH were already patched during install.
# post-fs-data.sh must NOT re-patch the live /vendor or /system files on every
# boot — doing so accumulates duplicate entries with each reboot.
#
# We only patch here when the overlay copy differs from the live file (i.e. if
# Magisk/KernelSU failed to overlay it and the live file is the unpatched
# original).  We detect this by checking whether our v4a entry is absent.

_patch_xml_aidl() {
  local FILE="$1"
  case $FILE in
    *audio_effects_config*.xml)
        # Remove all v4a variants before re-inserting — prevents accumulation
        sed -i "/v4a_standard_re/d;/v4a_standard_aidl/d;/v4a_aidl/d;/v4a_re/d" "$FILE"
        sed -i "0,/<libraries>/s|<libraries>|<libraries>\n        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"/>|" "$FILE"
        sed -i "0,/<effects>/s|<effects>|<effects>\n        <effect name=\"v4a_standard_aidl\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"/>|" "$FILE"
        ;;
    *.xml)
        sed -i "/v4a_standard_re/d;/v4a_standard_aidl/d;/v4a_aidl/d;/v4a_re/d" "$FILE"
        sed -i "0,/<libraries>/s|<libraries>|<libraries>\n        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"/>|" "$FILE"
        sed -i "0,/<effects>/s|<effects>|<effects>\n        <effect name=\"v4a_standard_re\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"/>|" "$FILE"
        ;;
    *.conf)
        sed -i "/v4a_standard_re {/,/}/d" "$FILE"
        sed -i "/v4a_aidl {/,/}/d" "$FILE"
        sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_aidl\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$FILE"
        sed -i "s/^libraries {/libraries {\n  v4a_aidl {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_aidl.so\n  }/g" "$FILE"
        ;;
  esac
}

_patch_xml_legacy() {
  local FILE="$1"
  case $FILE in
    *.xml)
        sed -i "/v4a_standard_re/d;/v4a_aidl/d;/v4a_re/d" "$FILE"
        sed -i "0,/<libraries>/s|<libraries>|<libraries>\n        <library name=\"v4a_re\" path=\"libv4a_re.so\"/>|" "$FILE"
        sed -i "0,/<effects>/s|<effects>|<effects>\n        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"/>|" "$FILE"
        ;;
    *.conf)
        sed -i "/v4a_standard_re {/,/}/d" "$FILE"
        sed -i "/v4a_re {/,/}/d" "$FILE"
        sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" "$FILE"
        sed -i "s/^libraries {/libraries {\n  v4a_re {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_re.so\n  }/g" "$FILE"
        ;;
  esac
}

if [ "$MODE" = "aidl" ]; then
  # Patch any live file that either lacks our entry entirely OR contains a
  # corrupted/wrong entry (wrong UUID, duplicate lines, or legacy v4a_re remnants).
  # A file is considered clean only when it has exactly one correct AIDL entry
  # with the right type UUID (7261726f).
  CFGS="$(find /odm /system /vendor /product /system_ext -type f \
          -name "*audio_effects*.conf" -o \
          -name "*audio_effects*.xml" -o \
          -name "*audio_effects_config*.xml" 2>/dev/null)"
  for FILE in ${CFGS}; do
    # Skip only if the correct UUID is already present (not just any v4a keyword)
    grep -q "7261726f-6d75-7369-6364-28e2fd3ac39e" "$FILE" 2>/dev/null && continue
    _patch_xml_aidl "$FILE"
  done

  if [ -d "/odm/etc/" ]; then
    echo "Binding audio_effects.xml to odm partition..."
    mount -o bind /data/adb/modules/ViPER4Android-RE/odm/etc/audio_effects.xml /odm/etc/audio_effects.xml
  fi
else
  CFGS="$(find /odm /system /vendor -type f \
          -name "*audio_effects*.conf" -o \
          -name "*audio_effects*.xml" 2>/dev/null)"
  for FILE in ${CFGS}; do
    # Skip only if the correct legacy impl UUID is already present
    grep -q "90380da3-8536-4744-a6a3-5731970e640f" "$FILE" 2>/dev/null && \
      ! grep -q "7261676f\|v4a_aidl" "$FILE" 2>/dev/null && continue
    _patch_xml_legacy "$FILE"
  done

  if [ -d "/odm/etc/" ]; then
    echo "Binding audio_effects.xml to odm partition..."
    mount -o bind /data/adb/modules/ViPER4Android-RE/odm/etc/audio_effects.xml /odm/etc/audio_effects.xml
  fi
fi
