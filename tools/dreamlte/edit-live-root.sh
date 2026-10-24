#!/bin/bash
# Edit the iOS root that is on the phone, offline, and write back only what
# changed. Run on the Mac; the editing happens in the Lima VM "s8" (Linux
# hfsplus, as root). The root must have been unmounted cleanly by iOS (a
# kernel booted with s8autoreset does that since commit 9521839): with a
# dirty HFS+ journal, XNU would replay old transactions over the edit.
#
#   tools/dreamlte/edit-live-root.sh LIVE.img STEP
#
# LIVE.img: the first 16 GiB of USERDATA, read from TWRP, e.g.
#   adb exec-out "dd if=/dev/block/sda24 bs=4194304 count=4096" > LIVE.img
# STEP: launchd       remove the REMOVED_JOBS of tools/sweet-root-tweaks.py
#       quiet         launchd, and disable BTAudioHALPlugin (idle CPU, galaxios8 c64-c68)
#       effects       Reduce Transparency + Reduce Motion
#       gputests      install s8malitest, s8gltest and libOSMesa (shim/dreamlte/out) with boot jobs
#       drop          install shim/dreamlte/out/s8drop (drop-zone runner), drop the gputests jobs
#       ca            install shim/dreamlte/out/libs8ca.dylib and insert it into backboardd
#                     (CoreAnimation on the Mali with boot-arg s8ca=1; tools/dreamlte/ca)
# Output: LIVE.img.edited and LIVE.img.changed.json (only the 32 MiB chunks
# that differ), for tools/dreamlte/flash-chunks.sh.
set -euo pipefail
live=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
step=${2:?launchd or effects}
repo=$(cd "$(dirname "$0")/../.." && pwd)
vm=/var/tmp/live-edit.img      # readable by limactl cp (runs as the user)

limactl shell s8 -- sudo bash -s "$live" "$step" "$repo" "$vm" <<'VM'
set -euo pipefail
live=$1 step=$2 repo=$3 vm=$4
modprobe hfsplus
[[ -f $vm && $(stat -c %s "$vm") == $(stat -c %s "$live") ]] && cmp -s "$live" "$vm" ||
	cp --sparse=always "$live" "$vm"
echo "== fsck before"
report=$(fsck.hfsplus -n "$vm" 2>&1 || true)
echo "$report" | grep -v 'Illegal name' | tail -4
if ! echo "$report" | grep -q 'appears to be OK'; then
	# Problems an iOS run leaves that fsck_hfs repairs safely (seen after the
	# first iOS boots: hard link counts). Anything else stops here.
	other=$(echo "$report" | grep -E '^   [A-Z]' | grep -vE 'Incorrect number of file hard links|Illegal name|The volume name is|Executing fsck_hfs' || true)
	if [[ -n $other ]]; then
		echo "fsck reports problems this script does not repair:" >&2
		echo "$other" >&2
		exit 1
	fi
	echo "== repairing known benign problems first"
	fsck.hfsplus -f -y "$vm" | grep -v 'Illegal name' | tail -3 || true
	fsck.hfsplus -f -n "$vm" | tail -1
fi
mnt=$(mktemp -d)
mount -t hfsplus -o rw,force,loop "$vm" "$mnt"
trap 'umount "$mnt" 2>/dev/null || true; rmdir "$mnt"' EXIT
python3 - "$repo/tools" "$mnt" "$step" <<'PY'
import importlib.util, os, sys
tools, root, step = sys.argv[1:4]
sys.argv = [sys.argv[0], root]          # the module reads its flags at import
spec = importlib.util.spec_from_file_location('tweaks', os.path.join(tools, 'sweet-root-tweaks.py'))
t = importlib.util.module_from_spec(spec)
spec.loader.exec_module(t)
if step == 'launchd':
    t.launchd(root)
elif step == 'quiet':
    t.launchd(root)
    t.bt_audio(root)
elif step == 'effects':
    t.reduce_effects(root)
elif step == 'drop':
    t.drop(root, os.path.join(tools, '..', 'shim/dreamlte/out'))
elif step == 'ca':
    t.ca(root, os.path.join(tools, '..', 'shim/dreamlte/out'))
elif step == 'gputests':
    t.gputests(root, os.path.join(tools, '..', 'shim/dreamlte/out'))
else:
    sys.exit('unknown step ' + step)
PY
sync
umount "$mnt"
trap - EXIT
rmdir "$mnt"
echo "== fsck after"
fsck.hfsplus -f -y "$vm" | grep -v 'Illegal name' | tail -3 || true
fsck.hfsplus -f -n "$vm" | tail -2
chmod 644 "$vm"
VM

# Only the changed 32 MiB chunks come back to the Mac, into a sparse $live.edited (a full
# 16 GiB copy could fill the Mac's disk); the comparison runs in the VM.
limactl shell s8 -- sudo bash -s "$live" "$vm" <<'VM'
set -euo pipefail
live=$1 vm=$2
rm -rf /var/tmp/live-chunks
mkdir -p /var/tmp/live-chunks
python3 - "$live" "$vm" <<'PY'
import hashlib, json, sys
a, b = sys.argv[1], sys.argv[2]
size = 32 << 20
zero = bytes(size)
chunks = []
with open(a, 'rb') as fa, open(b, 'rb') as fb:
    i = 0
    while True:
        x, y = fa.read(size), fb.read(size)
        if not y:
            break
        if x != y:
            chunks.append({'i': i, 'sha': hashlib.sha256(y).hexdigest(), 'zero': y == zero})
            open(f'/var/tmp/live-chunks/{i}.bin', 'wb').write(y)
        i += 1
json.dump({'chunk_mib': 32, 'chunks': chunks, 'zero_sha': hashlib.sha256(zero).hexdigest()},
          open('/var/tmp/live-chunks/changed.json', 'w'))
print(f'{len(chunks)} changed chunks: {[c["i"] for c in chunks]}')
PY
chmod -R a+r /var/tmp/live-chunks
VM
rm -f "$live.edited" "$live.changed".*.progress
size=$(stat -f %z "$live")
mkfile -n "$size" "$live.edited"             # sparse: only the chunks below take space
limactl cp s8:/var/tmp/live-chunks/changed.json "$live.changed.json.tmp"
for i in $(python3 -c 'import json,sys; print(" ".join(str(c["i"]) for c in json.load(open(sys.argv[1]))["chunks"]))' "$live.changed.json.tmp"); do
	rm -f "$live.chunk.tmp"                   # limactl cp does not replace an existing file
	limactl cp "s8:/var/tmp/live-chunks/$i.bin" "$live.chunk.tmp"
	want=$(python3 -c 'import json,sys; print([c["sha"] for c in json.load(open(sys.argv[1]))["chunks"] if c["i"] == int(sys.argv[2])][0])' "$live.changed.json.tmp" "$i")
	[[ $(shasum -a 256 < "$live.chunk.tmp" | cut -c1-64) == "$want" ]] || { echo "chunk $i came back wrong" >&2; exit 1; }
	dd if="$live.chunk.tmp" of="$live.edited" bs=1048576 seek=$((i * 32)) conv=notrunc 2>/dev/null
done
rm -f "$live.chunk.tmp"
python3 - "$live" <<'PY'
import json, sys
live = sys.argv[1]
m = json.load(open(live + '.changed.json.tmp'))
m['image'] = live + '.edited'
json.dump(m, open(live + '.changed.json', 'w'))
print(f'{len(m["chunks"])} changed chunks: {[c["i"] for c in m["chunks"]]}')
PY
rm -f "$live.changed.json.tmp"
limactl shell s8 -- sudo bash -c 'rm -rf /var/tmp/live-chunks; fstrim -v / || true'
