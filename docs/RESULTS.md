# galaxios8: results (2026-10-20 to 2026-10-24)

What this fork achieved running **iOS 16.7.16 (iPhone X, 20H392)** on a **Galaxy S8 SM-G950N**
(Exynos 8895), where it stopped, and why. The day-by-day log with every boot, number and dead
end is [`DREAMLTE_PROGRESS.md`](../DREAMLTE_PROGRESS.md); this page is the summary.

**Status: stopped on 2026-10-24.** After many test cycles the user ended the experiment at the
limits listed below. The test phone was given back to Android (stock BOOT, empty USERDATA) and
the build artefacts were removed from the Mac. Everything needed to rebuild is in this repository.

How claims are labelled: *phone log* = read from the kernel log after the run; *screenshot* =
the window server's framebuffer captured on the phone (`s8shot`); *user* = seen or said by the
person holding the phone; *built* = compiled but not run.

## What was reached

### Platform

| Area | Result | Evidence |
| --- | --- | --- |
| Boot chain | Stock S-Boot → our loader in BOOT (`sda7`) → XNU (xnu-8796) → HFS+ iOS root in USERDATA (`sda24`) | phone log, user |
| SM-G950N | Same touch IC, fuel gauge, UFS, panel and partition numbers as the G950F; iOS boots to Settings with touch (Settings → About shows iOS 16.7.16) | user photo (boot g2) |
| All 8 cores | 4 Cortex-A53 + 4 Mongoose. Root cause of the dead Mongoose cluster: S-Boot leaves the GIC distributor off, and the PMU only powers a core on a GIC wake request. With `GICD_CTLR` = 1, one PSCI `CPU_ON` per core works. Also needed: SweetCPU matching `samsung,mongoose-m2`, and all cores in one scheduler pset | phone log (K2c: `8/8 CPUs initialized`) |
| CPU clocks | Through the APM firmware (ACPM): the loader loads Samsung's DVFS plugin, requests go on channel 0. A53 1053 → **1690 MHz**, Mongoose 1066 → **1703 MHz** (2002 MHz works but reaches 81 °C in a minute). Thermal guard from the TMU: steps down at 80 °C | phone log (c10-c12) |
| RAM | **2 GiB** (was 512 MiB). Fixes: iOS VM sizing instead of macOS (the compressor reserved 16× RAM of VA), and 16 KiB-aligned commpage slots | phone log (c23a-c) |
| Display | 1125×2436 rendered and scaled to 1367×2960 by a kernel presenter thread at 60 Hz (CPU copy ~9 ms). Hardware scaling in the DPP (VGF0) works but tears, so it stays off | phone log, user video (c24-c28) |
| Unattended testing | Phone back in TWRP by itself: `s8autoreset` (clean shutdown), early-panic reset, SoC watchdog kicker. Kernel log ring at the end of USERDATA. Drop zone at 20 GiB runs test programs without editing the root. `s8swipe` (virtual touch in backboardd, taps and swipe scripts) and `s8shot` (screenshots) | phone log |

### GPU: the Mali-G71 MP20 renders the iOS UI

No Apple GPU driver can run on a Mali, so the route was HackiOS's: CoreAnimation's old OpenGL ES
backend, an `EAGLContext` replacement on Mesa, and our own kernel driver.

| Step | Result | Evidence |
| --- | --- | --- |
| Probe and first job | GPU_ID `0x60a00001`: Mali-G71 r0p0, 20 shader cores. A WRITE_VALUE job through our own page tables | phone log (c29, c30b) |
| `S8Mali` kernel driver | Power, MMU (4-level LPAE), BOs in a carve-out, synchronous jobs, fault recovery (soft reset), idle power-down after 200 ms. Self-test: 1000 jobs, 22 µs each | phone log (c31-c32, c41) |
| User client | Panfrost-like UAPI as IOKit selectors. 2000 jobs from an iOS process, 0 bad | phone log (c33) |
| Mesa 25.0.7 for iOS | OSMesa + Panfrost cross-built on the Mac, with a libdrm stand-in (`tools/dreamlte/mesa/s8drm`). **First OpenGL on the Mali from iOS**: correct pixels, 1.1 ms per 64-triangle frame into a texture, 90 s soak with 0 wrong pixels | phone log (c39, c40) |
| IOSurfaces | Imported zero-copy into the GPU's address space (selector 8), with cache maintenance around jobs. GL renders into a 1125×2436 IOSurface | phone log (c42, c43) |
| CoreAnimation on the Mali | Kernel inserts `libs8ca` into backboardd. CoreAnimation picks OpenGL ES 3.1 on "Mali-G71 (Panfrost)". Lock screen correct | screenshot (c51) |
| Picture fixed | Icons and pictures: SweetSurface now wraps CoreAnimation's own memory (`IOSurfaceAddress`). Rounded shapes: on this G71, Panfrost's quad-neighbour reads give 0, so `dFdx` = 0 and `fwidth` is wrong. `fwidth()` is now computed from the texture unit's mip-level choice. Home screen, dock, App Library and Cover Sheet drawn right | screenshot (c61), user: "much smoother" |
| Stable | backboardd crashed after a while: Mesa imports each surface twice, and the stand-in leaked one kernel import each time until an import failed. Fixed by handing back the existing handle, as Linux does. 23 app launches in a row, no crash | phone log (c62), user: "no more freezes" (gpu-c) |
| Apps | Settings, App Store and Safari drawn by the GPU | screenshot (c62) |

### Heat and battery

| Change | Result | Evidence |
| --- | --- | --- |
| Busy daemons found | With the phone idle, about 3 CPUs were busy. vmd (Visual Voicemail) and BTAudioHALPlugin in mediaserverd were reconnecting without pause to CommCenter and bluetoothd, which this port removes. launchd and logd were busy serving them. Found with `tools/dreamlte/s8top.c` and the kernel stack sampler | phone log (c64-c68) |
| Root fix | vmd job removed, BTAudioHALPlugin disabled. CPU time while idle **179 s → 8.8 s per minute** | phone log (c69) |
| Load governor (`s8gov=1`) | Per-CPU idle time from XNU's idle notifications. Every 40 ms each cluster gets the lowest clock that keeps it at most 80 % busy. Touch (1 s) and GPU submits (250 ms) boost to the ceiling at once | phone log (c63, c70) |
| Net effect | Under an app-launch soak: 69-73 °C → **45-55 °C**, 48 °C idle; panel copy as fast as with fixed clocks (~9.5 ms) | phone log (c62 vs c70/c71) |

## Where it stopped (limits)

- **No radios or peripherals**: Wi-Fi (BCM4361), Bluetooth, cellular, audio, cameras, sensors,
  GPS, vibration, SEP / Face ID. Their daemons had to be removed, and that causes knock-on
  problems (the busy loops above).
- **Apps that need missing pieces fail**: Photos aborts at every launch (an uncaught exception).
  Settings sometimes crashes: CoreImage in the app asks for OpenGL ES, and only backboardd has
  the Mesa bridge. Both bugs predate the GPU work.
- **Display**: the panel is fed by a CPU copy and scale (~9 ms per frame). Zero-copy scanout
  through the DPP tore (c24-c28) and was not solved.
- **GPU driver**: one shared GPU address space, jobs polled and synchronous, no tiler-heap growth,
  cache flushes of whole 11 MB surfaces per frame. One unexplained backboardd crash in c70 (a
  Mesa pool buffer's CPU mapping missing) that did not come back in c71.
- **RAM**: 2 of 4 GiB. XNU takes one contiguous range, and the S8's RAM is split in three.
- **Security is off**: permissive AMFI and sandbox, stub AppleKeyStore, dyld insertion allowed.
- **Last state not tried by hand**: gpu-e (kernel c70, touch/GPU boost) was booted for the user,
  who then stopped the experiment. gpu-d (governor without boost) felt like it dropped frames.
  gpu-c (fixed clocks) was smooth and stable but hot.

## Last configuration (for a restart)

- Kernel: `xnu/patches/0124-dreamlte-s8.patch` (generated by `tools/dreamlte/apply-dreamlte.py`),
  built with `tools/dreamlte/build-xnu-local.sh`, at commit `b8bf7f8` = "c70".
- Boot-args of gpu-e: `debug=0xc serial=3 keepsyms=1 -enable_kprintf_spam -noprogress
  rd=ufsroot0 ufsroot=USERDATA unentitled_ios_sim_launch=1 ignition_level=1
  wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0
  a5fb_noconsole=1 sweetfb_native=0 sweetfb_scale=1 cpus=8 sweetfb_hz=60 s8lit=1690000
  s8big=1703000 s8mali=1 s8ca=1 s8gov=1`, built with `S8_DRAM_MIB=2048 tools/dreamlte/make-boot.sh`.
- Root: `tools/mkiosroot.sh` + `finish-iosroot.sh` + `tools/sweet-root-tweaks.py`, then the
  `drop`, `ca` and `quiet` steps of `tools/dreamlte/edit-live-root.sh`.
- Drop zone: Mesa (`tools/dreamlte/mesa/mk-mesa.sh`) and the bridge (`tools/dreamlte/ca/build.sh`)
  as `ca-libOSMesa.8.dylib`, `ca-libs8gl.dylib`, `ca-ready` (`mkdrop.py`, `drop.sh`).
- Tools: `tools/dreamlte/cycle.sh` (flash, boot, back in TWRP, pull logs), `pull-shot.py`,
  `s8top.c`, `s8shot.c`, `ca/s8swipe.c`.

Not kept: the IPSW, the iOS root images, the build trees, the Lima VM and the phone's partition
backups (BOOT/EFS/RECOVERY) were all deleted at the user's request. A restart has to download
and rebuild them, and should take a fresh EFS backup first.
