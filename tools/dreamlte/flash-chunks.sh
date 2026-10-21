#!/bin/bash
# macOS/Linux port of flash-chunks.ps1: write an image to BOOT or USERDATA of
# the Galaxy S8 in TWRP, in verified chunks.
#
#   ANDROID_SERIAL=<serial> flash-chunks.sh IMAGE MANIFEST PARTITION [-y]
#
# MANIFEST comes from chunk-manifest.py. PARTITION is BOOT or USERDATA only;
# they are resolved to sda7 / sda24 and checked against the partition size.
# Each chunk is pushed to /tmp, dd'ed at its offset and the SHA-256 of what is
# now on the partition is compared. Verified chunk indexes are kept in a
# progress file next to the manifest, so after a USB drop just run it again.
# All-zero chunks are written from /dev/zero on the phone.
# Writes nothing outside the named partition. It DESTROYS that partition.
set -euo pipefail

img=${1:?image} manifest=${2:?manifest} part=${3:?BOOT or USERDATA}
yes=${4:-}
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the S8 serial (adb devices -l)}"
export ANDROID_SERIAL
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1; else shasum -a 256 | cut -d' ' -f1; fi; }

case $part in
	BOOT) dev=/dev/block/sda7 ;;
	USERDATA) dev=/dev/block/sda24 ;;
	*) echo "only BOOT or USERDATA can be written" >&2; exit 2 ;;
esac

model=$(adb shell getprop ro.product.device | tr -d '\r')
state=$(adb get-state | tr -d '\r')
[[ $model == dreamlte && $state == recovery ]] || { echo "expected dreamlte in recovery, got '$model' / '$state'" >&2; exit 1; }
adb shell "test -b $dev"

read -r mib n < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["chunk_mib"], len(m["chunks"]))' "$manifest")
size=$((mib << 20))
psize=$(adb shell "blockdev --getsize64 $dev" | tr -d '\r')
(( psize >= n * size )) || { echo "$dev is $psize bytes, image needs $((n * size))" >&2; exit 1; }
echo "target $part = $dev ($psize bytes), $n chunks of $mib MiB, serial $ANDROID_SERIAL"
if [[ $yes != -y ]]; then
	read -r -p "This ERASES $part on the phone above. Type the partition name to continue: " a
	[[ $a == "$part" ]] || { echo aborted >&2; exit 1; }
fi
adb shell "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true"

progress=${manifest%.json}.$part.progress
touch "$progress"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

python3 -c '
import json,sys
for c in json.load(open(sys.argv[1]))["chunks"]:
    print(c["i"], c["sha"], 1 if c["zero"] else 0)' "$manifest" |
while read -r i sha zero; do
	grep -qx "$i" "$progress" && continue
	for try in 1 2 3 4 5 6 7 8; do
		ok=0
		if [[ $zero == 1 ]]; then
			adb shell "dd if=/dev/zero of=$dev bs=1048576 seek=$((i * mib)) count=$mib conv=notrunc 2>/dev/null" && ok=1
		else
			dd if="$img" of="$tmp" bs=1048576 skip=$((i * mib)) count="$mib" 2>/dev/null
			if adb push "$tmp" /tmp/c.bin >/dev/null && \
			   adb shell "dd if=/tmp/c.bin of=$dev bs=1048576 seek=$((i * mib)) conv=notrunc 2>/dev/null; rm -f /tmp/c.bin"; then ok=1; fi
		fi
		if (( ok )); then
			got=$(adb shell "dd if=$dev bs=1048576 skip=$((i * mib)) count=$mib 2>/dev/null | sha256sum" | tr -d '\r' | cut -d' ' -f1)
			if [[ $got == "$sha" ]]; then echo "$i" >> "$progress"; break; fi
			echo "chunk $i try $try: verify mismatch ($got)" >&2
		else
			echo "chunk $i try $try failed" >&2
		fi
		(( try < 8 )) || { echo "chunk $i failed 8 times" >&2; exit 1; }
		for _ in $(seq 15); do adb get-state >/dev/null 2>&1 && break; sleep 2; done
	done
	if (( i % 16 == 0 )); then echo "chunk $((i + 1))/$n"; fi
done
adb shell sync
echo "all $n chunks written and verified on $dev"
