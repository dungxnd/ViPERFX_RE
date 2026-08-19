echo -n $LIBPATCH > $MODPATH/libpatch.txt

# ── AIDL detection ────────────────────────────────────────────────────────────
# Three signals, any one is sufficient:
#   1. init.svc.vendor.audio-hal-aidl = "running"
#      AOSP-standardized service name (Android 14+, audioserver.rc).
#      Zero-cost getprop; works on all AOSP-based OEMs using the canonical name.
#   2. init.svc.vendor.audio-effect-hal-aidl = "running"
#      Covers devices where the AIDL effects HAL runs as a separate service.
#   3. ps -A fallback: catches OEM binaries whose service name differs but whose
#      binary path still contains both "audio" and "aidl" in either order.
#      Pattern matches /apex/.../android.hardware.audio.service-aidl.example,
#      /vendor/bin/hw/android.hardware.audio-aidl, custom names, etc.
# API guard skips the (slow) ps scan on ancient devices but is not a hard gate.
ui_print "- Detecting audio HAL type..."
USE_AIDL=false
# Signal 1 & 2: O(1) property lookup — most reliable
if [ "$(getprop init.svc.vendor.audio-hal-aidl 2>/dev/null)" = "running" ] || \
   [ "$(getprop init.svc.vendor.audio-effect-hal-aidl 2>/dev/null)" = "running" ]; then
  USE_AIDL=true
fi
# Signal 3: ps scan fallback (only if signals 1/2 missed and device is recent enough)
if ! $USE_AIDL && [ "$API" -ge 33 ]; then
  if ps -A 2>/dev/null | grep -qE '([Aa]udio[^[:space:]]*[Aa]idl|[Aa]idl[^[:space:]]*[Aa]udio)'; then
    USE_AIDL=true
  fi
fi

if $USE_AIDL; then
  ui_print "    AIDL audio HAL detected — installing AIDL variant"
  echo "aidl" > $MODPATH/aidl_mode.txt
  sed -i 's/^name=.*/name=ViPER4Android Driver (AIDL)/' $MODPATH/module.prop

  ui_print "    Copying AIDL lib files..."
  cp_ch -n $MODPATH/common/files/libv4a_aidl_$ABI32.so $MODPATH$LIBDIR/lib/soundfx/libv4a_aidl.so
  if [ "$IS64BIT" ]; then
    cp_ch -n $MODPATH/common/files/libv4a_aidl_$ABI.so $MODPATH$LIBDIR/lib64/soundfx/libv4a_aidl.so
  fi

  ui_print "    Patching audio_effects config files (AIDL)"
  CFGS="$(find /odm /system /vendor /product /system_ext -type f -name "*audio_effects*.conf" -o -name "*audio_effects*.xml" -o -name "*audio_effects_config*.xml")"
  for OFILE in ${CFGS}; do
    FILE="$MODPATH$(echo $OFILE | sed "s|^/vendor|/system/vendor|g" | sed "s|^/odm|/system/vendor/odm|g" | sed "s|^/product|/system/product|g" | sed "s|^/system_ext|/system/system_ext|g")"
    cp_ch -n $OFILE $FILE
    case $FILE in
      *.conf)
          sed -i "/v4a_standard_re {/,/}/d" $FILE
          sed -i "/v4a_aidl {/,/}/d" $FILE
          sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_aidl\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" $FILE
          sed -i "s/^libraries {/libraries {\n  v4a_aidl {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_aidl.so\n  }/g" $FILE
          ;;
      *audio_effects_config*.xml)
          sed -i "/v4a_standard_re/d" $FILE
          sed -i "/v4a_standard_aidl/d" $FILE
          sed -i "/v4a_aidl/d" $FILE
          sed -i "/<libraries>/ a\        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"\/>" $FILE
          sed -i "/<effects>/ a\        <effect name=\"v4a_standard_aidl\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"\/>" $FILE
          ;;
      *.xml)
          sed -i "/v4a_standard_re/d" $FILE
          sed -i "/v4a_standard_aidl/d" $FILE
          sed -i "/v4a_aidl/d" $FILE
          sed -i "/<libraries>/ a\        <library name=\"v4a_aidl\" path=\"libv4a_aidl.so\"\/>" $FILE
          sed -i "/<effects>/ a\        <effect name=\"v4a_standard_re\" library=\"v4a_aidl\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\" type=\"7261726f-6d75-7369-6364-28e2fd3ac39e\"\/>" $FILE
          ;;
    esac
  done

else
  ui_print "    Legacy audio HAL detected — installing non-AIDL variant"
  echo "legacy" > $MODPATH/aidl_mode.txt
  sed -i 's/^name=.*/name=ViPER4Android Driver (Legacy)/' $MODPATH/module.prop

  ui_print "    Copying lib files..."
  cp_ch -n $MODPATH/common/files/libv4a_re_$ABI32.so $MODPATH$LIBDIR/lib/soundfx/libv4a_re.so
  if [ "$IS64BIT" ]; then
    cp_ch -n $MODPATH/common/files/libv4a_re_$ABI.so $MODPATH$LIBDIR/lib64/soundfx/libv4a_re.so
  fi

  ui_print "    Patching audio_effects config files"
  CFGS="$(find /odm /system /vendor -type f -name "*audio_effects*.conf" -o -name "*audio_effects*.xml")"
  for OFILE in ${CFGS}; do
    FILE="$MODPATH$(echo $OFILE | sed "s|^/vendor|/system/vendor|g")"
    cp_ch -n $OFILE $FILE
    case $FILE in
      *.conf)
          sed -i "/v4a_standard_re {/,/}/d" $FILE
          sed -i "/v4a_re {/,/}/d" $FILE
          sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" $FILE
          sed -i "s/^libraries {/libraries {\n  v4a_re {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_re.so\n  }/g" $FILE
          ;;
      *.xml)
          sed -i "/v4a_standard_re/d" $FILE
          sed -i "/v4a_re/d" $FILE
          sed -i "/<libraries>/ a\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" $FILE
          sed -i "/<effects>/ a\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" $FILE
          ;;
    esac
  done
fi
