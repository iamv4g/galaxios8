# CoreAnimation on the Mali (backboardd bridge)

CoreAnimation's render server (`CA::WindowServer::EAGLServer::renderer()`, QuartzCore 16.7.16)
tries Metal, then `[[EAGLContext alloc] initWithAPI:3 properties:]` with `CA::OGL::GLESContext`,
then its software rasterizer. There is no Metal device on the S8 and iOS's OpenGLES has no driver
for the Mali, so the UI is drawn by the CPU. This directory puts an OpenGL ES implementation of
our own behind `EAGLContext` inside backboardd: Mesa 25.0.7 Panfrost on the S8Mali kernel driver.

| File | What |
|---|---|
| `s8ca.c` | `libs8ca.dylib`, ours: inserted into backboardd; with boot-arg `s8ca=1` sends stderr to the kernel log (S8Mali selector 10, lines `S8 LOG backboardd:`), unpacks the drop-zone files `ca-*` to `/private/var/tmp/s8ca/`, loads `libs8gl.dylib`, logs crashes |
| `eagl.c`, `gl_osmesa.c`, `redwood_gl.h` | `libs8gl.dylib`: HackiOS's EAGLContext bridge (MIT, `LICENSE.hackios`, commit a79ff07); our changes are marked `galaxios8` |
| `eagl_slots.txt`, `mkeagl.py` | HackiOS's table of OpenGLES's macro-context dispatch offsets and its generator. All 414 offsets were checked against the 16.7.16 (20H392) OpenGLES in the dyld cache: identical to 16.5 |
| `build.sh` | builds both (needs `tools/dreamlte/mesa/mk-mesa.sh` output) |

How it is put together:

1. Kernel (0124): a MAC policy named `AMFI` answers dyld's "check dyld policy" request (0x5a)
   with everything allowed, so dyld honours `DYLD_INSERT_LIBRARIES` and a library that fails to
   load does not kill the process (`s8dyld=0` turns it off). S8Mali selector 10 logs text.
2. Mesa (`patch-mesa.py`): `OSMESA_ES_PROFILE` (OpenGL ES 2/3 contexts), as HackiOS's
   `osmesa-es-profile.patch`; IOSurfaces become textures/render targets through
   `glEGLImageTargetTexture2DOES(target, surfaceID)` (c43).
3. Root, once: `edit-live-root.sh LIVE.img ca` installs `/usr/local/lib/libs8ca.dylib` and adds
   `DYLD_INSERT_LIBRARIES` to backboardd's job. Old kernels ignore the variable; kernels without
   `s8ca=1` load the library but it returns at once.
4. Each try: `build.sh`, `mkdrop.py OUT.img shim/dreamlte/out/drop/ca-libs8gl.dylib
   shim/dreamlte/out/drop/ca-libOSMesa.8.dylib`, `drop.sh OUT.img`, boot an image with `s8ca=1`.

eagl.c reads its own boot-args (`glframes=1`, `gltrace=`, `glcensus=1`, `glwait=`, ...; see the
top of the file).
