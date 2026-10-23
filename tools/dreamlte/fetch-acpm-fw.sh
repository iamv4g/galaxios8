#!/bin/bash
# Samsung's ACPM DVFS plugin (exynos8895_acpm_dvfs.fw) for the S8 loader, out
# of git. TWRP's kernel carries it built in and uploads it to the APM at boot;
# the loader does the same under our boot (shim/dreamlte/pmic.c). Taken from
# the RECOVERY backup (docs/mac-build.md step 2), i.e. the exact blob this
# phone's TWRP uses; the same bytes are firmware/exynos8895_acpm_dvfs.fw.ihex
# in Samsung's universal8895 kernel source.
#   tools/dreamlte/fetch-acpm-fw.sh [RECOVERY.img]
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
img=${1:-$repo/backup/RECOVERY.img}
out=$repo/shim/dreamlte/firmware/exynos8895_acpm_dvfs.fw
mkdir -p "$(dirname "$out")"
python3 - "$img" "$out" <<'PY'
import hashlib, sys
SHA = 'a272830513378dc2fc5bfa23a3d08dbff38fffdf47b5b6c3de572cd4d0eea7e2'
SIZE = 23552
HEAD = bytes.fromhex('08b5094b01221a60084b0022c360084b')
d = open(sys.argv[1], 'rb').read()
at = d.find(HEAD)
while at >= 0:
    blob = d[at:at + SIZE]
    if hashlib.sha256(blob).hexdigest() == SHA:
        open(sys.argv[2], 'wb').write(blob)
        print(f'{sys.argv[2]}: {SIZE} bytes from offset {at:#x}')
        sys.exit(0)
    at = d.find(HEAD, at + 1)
sys.exit('blob not found (TWRP kernel compressed or a different build?)')
PY
