#!/bin/bash
# macOS/Linux port of pull-sweetlog.ps1. Read-only on the phone (TWRP).
#   ANDROID_SERIAL=<serial> pull-sweetlog.sh NAME
# -> logs/sweetlog-NAME.txt (kernel console ring: last 1025 blocks of USERDATA)
#    logs/pmsg-NAME.txt     (loader + early kernel text, ramoops pmsg)
set -euo pipefail
name=${1:?name, e.g. boot32}
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the S8 serial (adb devices -l)}"
export ANDROID_SERIAL
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$repo/logs"
adb shell 'b=$(blockdev --getsize64 /dev/block/sda24); dd if=/dev/block/sda24 of=/tmp/slog.bin bs=4096 skip=$((b/4096-1025)) count=1025 2>/dev/null'
adb pull /tmp/slog.bin "$repo/logs/sweetlog-$name.bin" >/dev/null
adb pull /sys/fs/pstore/pmsg-ramoops-0 "$repo/logs/pmsg-$name.txt" >/dev/null
python3 "$repo/tools/sweet-log.py" "$repo/logs/sweetlog-$name.bin" "$repo/logs/sweetlog-$name.txt"
echo "kernel log: logs/sweetlog-$name.txt"
echo "pmsg log:   logs/pmsg-$name.txt"
