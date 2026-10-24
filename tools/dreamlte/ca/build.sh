#!/bin/bash
# Build the CoreAnimation bridge for backboardd on this Mac (README.md here):
#   shim/dreamlte/out/libs8ca.dylib            the injected loader, for the root (/usr/local/lib)
#   shim/dreamlte/out/drop/ca-libs8gl.dylib    the EAGLContext bridge, for the drop zone
#   shim/dreamlte/out/drop/ca-libOSMesa.8.dylib  Mesa (Panfrost) as the bridge links it
# Needs research/mesa-ios/out from tools/dreamlte/mesa/mk-mesa.sh (ca/, exported-gl.txt, gl.xml).
#
#   tools/dreamlte/ca/build.sh
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
here=$repo/tools/dreamlte/ca
M=$repo/research/mesa-ios/out
out=$repo/shim/dreamlte/out
obj=$out/ca-obj
mkdir -p "$obj" "$out/drop"
CC=(xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wno-unused-function)

python3 "$here/mkeagl.py" "$here/eagl_slots.txt" "$M/gl.xml" "$M/exported-gl.txt" "$here/eagl.c" > "$obj/eagl_gen.h"
"${CC[@]}" -dynamiclib -install_name /private/var/tmp/s8ca/libs8gl.dylib -I"$obj" -I"$here" \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$M/ca/libOSMesa.8.dylib" \
	-o "$obj/libs8gl.dylib" "$here/eagl.c" "$here/gl_osmesa.c"
"${CC[@]}" -dynamiclib -install_name /usr/local/lib/libs8ca.dylib -framework CoreFoundation -framework IOKit \
	-o "$out/libs8ca.dylib" "$here/s8ca.c"
otool -L "$obj/libs8gl.dylib" | grep -q /private/var/tmp/s8ca/libOSMesa.8.dylib
codesign -f -s - "$obj/libs8gl.dylib" "$out/libs8ca.dylib"
cp "$obj/libs8gl.dylib" "$out/drop/ca-libs8gl.dylib"
cp "$M/ca/libOSMesa.8.dylib" "$out/drop/ca-libOSMesa.8.dylib"
ls -l "$out/libs8ca.dylib" "$out/drop/ca-libs8gl.dylib" "$out/drop/ca-libOSMesa.8.dylib"
