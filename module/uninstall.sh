#!/system/bin/sh
# Clean up v4a shared memory files left by previous installs.
V4A_SHM_DIR=/data/local/tmp/v4a
if [ -d "$V4A_SHM_DIR" ]; then
  rm -rf "$V4A_SHM_DIR"
fi