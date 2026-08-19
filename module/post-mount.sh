#!/system/bin/sh
# post-mount.sh — KernelSU / KernelSU Next only.
# Runs AFTER the metamodule (meta-overlayfs) has completed its OverlayFS mounts.
# Magisk and APatch do not call this file; it is silently ignored by them.
# Use this for any logic that requires the module's overlaid files to already
# be visible at their final paths.

MODDIR=${0%/*}

# Nothing required here for now — audio effect configs are overlaid by the
# metamodule. This file exists as a documented extension point for future use
# (e.g., verifying overlay succeeded, triggering audioserver restart, etc.).
