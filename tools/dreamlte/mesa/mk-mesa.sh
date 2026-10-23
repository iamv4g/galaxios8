#!/bin/bash
# Build Mesa 25.0.7 for iOS on the Galaxy S8 on this Mac: OSMesa (OpenGL / OpenGL ES) with the
# Panfrost gallium driver for the Mali-G71 behind the S8Mali kernel driver, softpipe as the
# fallback. Cross-compiled with Xcode's clang against the iPhoneOS SDK, in the git-ignored
# research/mesa-ios/. Nothing of Mesa goes into git; tools/dreamlte/mesa/ holds our changes:
#   s8drm/           the libdrm stand-in (DRM requests -> S8Mali user client)
#   patch-mesa.py    the edits to Mesa's tree (meson, mmap, OSMesa screen)
#
#   tools/dreamlte/mesa/mk-mesa.sh      -> research/mesa-ios/out/libOSMesa.8.dylib
#
# Needs: Xcode, Homebrew ninja and bison (keg-only, >= 3), python3 (meson and mako go into a
# venv in research/mesa-ios).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
here=$repo/tools/dreamlte/mesa
B=$repo/research/mesa-ios
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
mkdir -p "$B/out"
cd "$B"

[[ -x venv/bin/meson ]] || { python3 -m venv venv && venv/bin/pip -q install meson mako pyyaml packaging; }
export PATH=$B/venv/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:$PATH
[[ $(bison --version | head -1 | awk '{print $NF}') == 3.* ]] || { echo "need bison 3 (brew install bison)" >&2; exit 1; }

# Fresh tree whenever our changes differ from the ones it was made with.
stamp=$(cat "$here/patch-mesa.py" "$here"/s8drm/* | shasum -a 256 | cut -c1-64)
src=src/mesa-$VERSION
if [[ ! -d $src || $(cat "$src/.s8-stamp" 2>/dev/null) != "$stamp" ]]; then
	[[ -f mesa-$VERSION.tar.xz ]] || curl -sSLf -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf "$src" build
	mkdir -p src
	tar -xf mesa-$VERSION.tar.xz -C src
	cp -R "$here/s8drm" "$src/src/s8drm"
	python3 "$here/patch-mesa.py" "$src"
	echo "$stamp" > "$src/.s8-stamp"
fi

SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC=$(xcrun -sdk iphoneos -f clang)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
ar = '$(xcrun -f ar)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

[[ -f build/build.ninja ]] || meson setup build "$src" --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C build src/gallium/targets/osmesa/libOSMesa.8.dylib > ninja.log
cp build/src/gallium/targets/osmesa/libOSMesa.8.dylib out/libOSMesa.8.dylib
install_name_tool -id /usr/local/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
codesign -f -s - out/libOSMesa.8.dylib
# the same library for the drop zone (tools/dreamlte/s8drop.c), where s8drop extracts it
mkdir -p out/drop
cp out/libOSMesa.8.dylib out/drop/libOSMesa.8.dylib
install_name_tool -id /private/var/tmp/s8drop/libOSMesa.8.dylib out/drop/libOSMesa.8.dylib  # dyld refuses @ paths here (c36)
codesign -f -s - out/drop/libOSMesa.8.dylib
echo "mk-mesa.sh: $B/out/libOSMesa.8.dylib, out/drop/libOSMesa.8.dylib ($(stat -f %z out/libOSMesa.8.dylib) bytes)"
