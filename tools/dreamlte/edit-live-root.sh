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
#       effects       Reduce Transparency + Reduce Motion
#       malitest      install shim/dreamlte/out/s8malitest and its boot job
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
elif step == 'effects':
    t.reduce_effects(root)
elif step == 'malitest':
    t.malitest(root, os.path.join(tools, '..', 'shim/dreamlte/out/s8malitest'))
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

limactl cp "s8:$vm" "$live.edited"
python3 - "$live" "$live.edited" <<'PY'
import hashlib, json, sys
a, b = sys.argv[1], sys.argv[2]
size = 32 << 20
zero = bytes(size)
zsha = hashlib.sha256(zero).hexdigest()
chunks = []
with open(a, 'rb') as fa, open(b, 'rb') as fb:
    i = 0
    while True:
        x, y = fa.read(size), fb.read(size)
        if not y:
            break
        if x != y:
            chunks.append({'i': i, 'sha': hashlib.sha256(y).hexdigest(), 'zero': y == zero})
        i += 1
json.dump({'image': b, 'chunk_mib': 32, 'chunks': chunks, 'zero_sha': zsha},
          open(a + '.changed.json', 'w'))
print(f'{len(chunks)} changed chunks: {[c["i"] for c in chunks]}')
PY
