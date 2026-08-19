echo -n $LIBPATCH > $MODPATH/libpatch.txt

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
