#!/bin/bash
# List ARMv8.1 LSE instructions (illegal on Cortex-A53 / Exynos 8895) in a
# kernel Mach-O, counted per function. Usage: lse-scan.sh KERNEL
set -euo pipefail
# On macOS llvm-objdump lives in the Xcode toolchain, not on PATH. Without it
# the pipeline below would report "LSE instructions: 0" for anything.
objdump=$(command -v llvm-objdump || xcrun --find llvm-objdump 2>/dev/null) ||
	{ echo "lse-scan: no llvm-objdump" >&2; exit 1; }
"$objdump" -d --no-show-raw-insn "$1" |
	grep -E '^[0-9a-f]+ <|	(cas|casa|casal|casl|casp|ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|stadd|stclr|steor|stset|stsmax|stsmin|stumax|stumin|swp)[a-z]*	' |
	awk '/^[0-9a-f]+ </{fn=$2; next} {n++; c[fn]++} END {printf "LSE instructions: %d\n", n+0; for (f in c) print c[f], f}' |
	sort -rn | head -40
