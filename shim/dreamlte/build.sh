#!/bin/bash
# Build the Galaxy S8 XNU loader (Samsung S-Boot "kernel" Image).
# Optionally pack a BOOT image: build.sh STOCK_BOOT.img PAYLOAD.bin OUT.img
# (PAYLOAD from tools/mkpayload.py; STOCK_BOOT supplies header and DTBH).
set -euo pipefail
cd "$(dirname "$0")"
# Linux: aarch64-linux-gnu-*. macOS: brew install aarch64-elf-gcc aarch64-elf-binutils
# (bare-metal aarch64-elf is enough: the loader is freestanding).
for CROSS in aarch64-linux-gnu- aarch64-elf-; do
	command -v "${CROSS}gcc" >/dev/null 2>&1 && break
	CROSS=
done
[ -n "$CROSS" ] || { echo "error: no aarch64 cross compiler (see docs/mac-build.md)" >&2; exit 1; }
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
NM=${CROSS}nm
READELF=${CROSS}readelf
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi; }
filesize() { wc -c < "$1" | tr -d ' '; }
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"
# Extra defines, e.g. LOADER_CFLAGS=-DMNGS_DIAG=1 for the Mongoose experiment build.
CFLAGS="$CFLAGS ${LOADER_CFLAGS:-}"
# Samsung's ACPM DVFS plugin, not in git: tools/dreamlte/fetch-acpm-fw.sh.
# Without it the loader leaves the CPU clocks as S-Boot set them.
if [ -f firmware/exynos8895_acpm_dvfs.fw ]; then
	CFLAGS="$CFLAGS -DACPM_DVFS_FW=\"firmware/exynos8895_acpm_dvfs.fw\""
fi
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c loader.c -o out/loader.o
$CC $CFLAGS -c ../fdt.c -o out/fdt.o
$CC $CFLAGS -c ufs_probe.c -o out/ufs_probe.o
$CC $CFLAGS -c smp_probe.c -o out/smp_probe.o
$CC $CFLAGS -c touch_probe.c -o out/touch_probe.o
$CC $CFLAGS -c pmic.c -o out/pmic.o
$CC $CFLAGS -c display_probe.c -o out/display_probe.o
objs="out/loader.o out/fdt.o out/ufs_probe.o out/smp_probe.o out/touch_probe.o out/pmic.o out/display_probe.o"
# Runs wherever S-Boot puts it with no relocation pass.
abs=$($READELF -r $objs | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations" >&2; exit 1; }
$CC $CFLAGS -static -T loader.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o $objs -o out/loader.elf
$OBJCOPY -O binary out/loader.elf out/Image
end=$($NM out/loader.elf | awk '$3=="_end"{print $1}')
size=$((16#$end))
truncate -s "$size" out/Image
echo "out/Image: $(filesize out/Image) bytes"
if [ $# = 3 ]; then
	python3 ../../tools/dreamlte_mkboot.py "$1" out/Image "$2" "$3"
	sha256 "$3"
fi
