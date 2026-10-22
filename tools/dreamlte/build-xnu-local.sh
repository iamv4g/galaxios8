#!/bin/bash
# Build the S8 kernel on this Mac, the same way the `xnu` workflow does, in
# the git-ignored research/xnu-local/. Needs Homebrew jq, gum, cmake, ninja,
# Xcode (27.1 works: patch 0083 adds the IORPC.kernelContent field its iig
# expects) and Kernel Debug Kit 13.5 22G74 in /Library/Developer/KDKs.
#
#   tools/dreamlte/build-xnu-local.sh            -> prints the kernel path
#
# The first run fetches blacktop/darwin-xnu-build, xnu and the helper projects
# (~15 min on an Apple Silicon Mac); later runs reset the xnu tree, re-apply
# xnu/patches and rebuild what changed.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
W=$repo/research/xnu-local
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549   # as .github/workflows/xnu.yml

[[ -d /Library/Developer/KDKs/KDK_13.5_22G74.kdk ]] || { echo "missing KDK 13.5 22G74 (docs/mac-build.md)" >&2; exit 1; }
mkdir -p "$W/bin"
printf '#!/bin/sh\nexit 0\n' > "$W/bin/xcodes"      # build.sh only checks it exists
chmod +x "$W/bin/xcodes"

if [[ ! -d $W/dxb/.git ]]; then
	git init -q "$W/dxb"
	git -C "$W/dxb" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
	git -C "$W/dxb" checkout -q FETCH_HEAD
fi
# Fresh build script and patch set every time.
git -C "$W/dxb" checkout -q -- build.sh
rm -f "$W/dxb/patches/"0[0-9][0-9][0-9]-*.patch "$W/dxb/patches/"0[0-9][0-9][0-9]-*.sh
cp "$repo"/xnu/patches/*.patch "$repo"/xnu/patches/*.sh "$W/dxb/patches/"
# Cortex-A53 code generation for libfirehose_kernel (no LSE), as in CI.
sed -i '' "s/ARCHS=\"x86_64 arm64e\" VALID_ARCHS=\"x86_64 arm64e\"/ARCHS=arm64 VALID_ARCHS=arm64 OTHER_CFLAGS='-mkernel -nostdinc -Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto'/" "$W/dxb/build.sh"
grep -q "mcpu=cortex-a53 -march=armv8-a+crypto" "$W/dxb/build.sh"
# Patched files from the last run must not stay patched.
if [[ -d $W/dxb/xnu/.git ]]; then
	git -C "$W/dxb/xnu" checkout -q -- .
	git -C "$W/dxb/xnu" clean -qfd
fi

# build.sh skips xnu while build/xnu.obj/kernel.<type> exists ("already
# built"); without it, make reuses the object tree and rebuilds what changed.
rm -f "$W/dxb/build/xnu.obj/kernel.development.a5xelte" "$W/dxb/build/xnu.obj/kernel.development.a5xelte.link"
rm -rf "$W/dxb/fakeroot/System/Library/Kernels/kernel.development.a5xelte"*

cd "$W/dxb"
log=$W/build-$(date +%Y%m%d-%H%M%S).log
PATH=$W/bin:$PATH MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=A5XELTE \
	bash build.sh > "$log" 2>&1 || { tail -30 "$log" >&2; echo "build failed, log: $log" >&2; exit 1; }
k=$W/dxb/fakeroot/System/Library/Kernels/kernel.development.a5xelte
[[ -f $k ]] || { echo "no kernel produced, log: $log" >&2; exit 1; }
# One hit in K256 is expected: SHA-256 round constants in __TEXT_EXEC that
# disassemble as an LSE opcode. Anything else would trap on the A53.
scan=$(bash "$repo/tools/dreamlte/lse-scan.sh" "$k")
echo "$scan" | head -1 >&2
if echo "$scan" | grep -E '^ *[0-9]+ <' | grep -vq '<K256>'; then
	echo "warning: LSE atomics in $k:" >&2
	echo "$scan" >&2
fi
echo "$k"
