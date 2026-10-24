#!/bin/bash
# Write a drop-zone image (tools/dreamlte/mkdrop.py) to USERDATA at 20 GiB, after the 16 GiB
# iOS root and far from the kernel log ring at the end, and verify it. TWRP only, the S8 only.
#
#   ANDROID_SERIAL=<serial> tools/dreamlte/drop.sh DROP.img
set -euo pipefail
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the S8 serial (adb devices -l)}"
export ANDROID_SERIAL
img=${1:?drop image}
size=$(wc -c < "$img" | tr -d ' ')
(( size <= 268435456 && size % 4096 == 0 )) || { echo "drop image must be 4 KiB aligned, at most 256 MiB" >&2; exit 2; }
[[ $(adb get-state 2>/dev/null | tr -d '\r') == recovery ]] || { echo "phone not in recovery" >&2; exit 1; }
[[ $(adb shell getprop ro.product.device | tr -d '\r') == dreamlte ]] || { echo "not a dreamlte" >&2; exit 1; }
total=$(adb shell blockdev --getsize64 /dev/block/sda24 | tr -d '\r')
[[ $total == 58858668032 ]] || { echo "sda24 is not the 57 GB USERDATA ($total)" >&2; exit 1; }
seek=$(( (20 << 30) / 4096 ))
blocks=$(( size / 4096 ))
want=$(shasum -a 256 "$img" | cut -c1-64)
adb push "$img" /tmp/drop.img >/dev/null
adb shell "dd if=/tmp/drop.img of=/dev/block/sda24 bs=4096 seek=$seek conv=notrunc,fsync 2>/dev/null; sync; rm -f /tmp/drop.img" </dev/null
got=$(adb exec-out "dd if=/dev/block/sda24 bs=4096 skip=$seek count=$blocks 2>/dev/null" | shasum -a 256 | cut -c1-64)
[[ $got == "$want" ]] || { echo "read-back mismatch: $got" >&2; exit 1; }
echo "drop zone = $img ($size bytes, $want)"
