#!/bin/bash
# One unattended test cycle on the galaxios8 test phone (Galaxy S8 in TWRP):
# write a BOOT image to sda7, read it back, reboot, wait until the phone is
# back in TWRP (the image must end in a reset to recovery: a MNGS_DIAG loader,
# or a kernel booted with s8autoreset=N), then pull the pmsg zone and the
# kernel log ring into logs/.
#
#   tools/dreamlte/cycle.sh BOOT_IMAGE NAME [TIMEOUT_SECONDS]
#
# Safety: only the serial below, only when it reports dreamlte and is in
# recovery, only BOOT (sda7), and the write is verified byte for byte. If the
# phone does not come back within the timeout (default 600 s) the script
# stops and says so; it never retries a write.
set -euo pipefail

SERIAL=ce081718edd2030d02
img=${1:?BOOT image}
name=${2:?name for the logs, e.g. k2b}
timeout=${3:-600}
repo=$(cd "$(dirname "$0")/../.." && pwd)
export ANDROID_SERIAL=$SERIAL

[[ -f $img ]] || { echo "no such image: $img" >&2; exit 2; }
size=$(wc -c < "$img" | tr -d ' ')
(( size <= 41943040 )) || { echo "image larger than BOOT (40 MiB)" >&2; exit 2; }
[[ $(adb get-state 2>/dev/null | tr -d '\r') == recovery ]] || { echo "phone $SERIAL not in recovery" >&2; exit 1; }
[[ $(adb shell getprop ro.product.device | tr -d '\r') == dreamlte ]] || { echo "phone is not a dreamlte" >&2; exit 1; }
[[ $(adb shell blockdev --getsize64 /dev/block/sda7 | tr -d '\r') == 41943040 ]] || { echo "sda7 is not the 40 MiB BOOT" >&2; exit 1; }

want=$(shasum -a 256 "$img" | cut -c1-64)
adb push "$img" /tmp/boot.img >/dev/null
adb shell "dd if=/tmp/boot.img of=/dev/block/sda7 bs=1048576 conv=fsync 2>/dev/null; sync; rm -f /tmp/boot.img" </dev/null
got=$(adb exec-out "dd if=/dev/block/sda7 bs=$size count=1 2>/dev/null" | shasum -a 256 | cut -c1-64)
[[ $got == "$want" ]] || { echo "sda7 read-back mismatch: $got" >&2; exit 1; }
echo "sda7 = $img ($want)"

adb reboot
echo "rebooted at $(date +%T), waiting up to ${timeout}s for TWRP"
sleep 15
end=$((SECONDS + timeout))
until [[ $(adb get-state 2>/dev/null | tr -d '\r') == recovery ]]; do
	(( SECONDS < end )) || { echo "not back in TWRP after ${timeout}s: needs a person at the phone" >&2; exit 3; }
	sleep 3
done
echo "back in TWRP at $(date +%T)"

mkdir -p "$repo/logs"
adb pull /sys/fs/pstore/pmsg-ramoops-0 "$repo/logs/pmsg-$name.txt" >/dev/null 2>&1 ||
	echo "no pmsg zone this time" >&2
bash "$repo/tools/dreamlte/pull-sweetlog.sh" "$name" >/dev/null 2>&1 ||
	echo "log ring pull failed" >&2
ls -l "$repo/logs/"*"-$name."* 2>/dev/null
