# galaxios8: iOS on the Samsung Galaxy S8

**iOS 16.7 on the Samsung Galaxy S8, Exynos 8895 (`dreamlte`): SM-G950F and SM-G950N.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Galaxy S8. On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd and SpringBoard. The phone reaches the **iOS lock screen**.

> **Status (2026-10-24): experiment stopped.** This fork reached all 8 CPU cores, 2 GiB RAM, full CPU clocks with a load governor, and **the iOS UI drawn by the Mali-G71 GPU** (CoreAnimation → OpenGL ES → Mesa Panfrost → our XNU GPU driver), with apps such as Settings, App Store and Safari. It then stopped at the limits in [docs/RESULTS.md](docs/RESULTS.md); the test phone went back to Android. Summary of results: **[docs/RESULTS.md](docs/RESULTS.md)**. Full log: [DREAMLTE_PROGRESS.md](DREAMLTE_PROGRESS.md).

> **2026-10-22:** iOS 16.7.16 also boots on the Korean **SM-G950N** (same touch IC, fuel gauge, UFS, panel and partition numbers as the G950F), built entirely on a Mac (see [docs/mac-build.md](docs/mac-build.md)).

> Fork of [Floppikrob/ios-galaxy-s8](https://github.com/Floppikrob/ios-galaxy-s8) (kept as the `upstream` remote). Port history up to boot 31 is from that project; see [Authors](#authors).

> ⚠️ Experimental bring-up, not a usable phone OS. Much is missing (see [Known bugs](#known-bugs)). Installing it **erases the USERDATA partition** (all Android data). Exynos S8 only: see [Supported models](#supported-models).

## Supported models

| Model | SoC | State |
| --- | --- | --- |
| **SM-G950F** (international) | Exynos 8895 | Upstream port: lock screen, home screen, touch, keys, battery |
| **SM-G950N** (Korea) | Exynos 8895 | This fork (bootloader `G950NKSU3CRJ1`): 8 cores, 2 GiB, GPU-drawn UI, apps |
| Other Exynos S8 variants (e.g. G950FD) | Exynos 8895 | Untested; probably the same board (`dreamlte`) |
| SM-G950U/U1/W/0/SC | Snapdragon 835 | **Not supported** (different SoC) |
| Galaxy S8+ (SM-G955x) | | **Not supported** (different panel; partitions not checked) |

The phone's Android props can lie (for example a ROM ported from another model). Check the real hardware in TWRP or Android: `/proc/device-tree/model` must say `DREAMLTE` / `EXYNOS8895`, and `ro.boot.em.model` or the bootloader version (`G950F…`, `G950N…`) gives the model.

## What works

State at the end of this fork (2026-10-24). Details and evidence: [docs/RESULTS.md](docs/RESULTS.md).

| Area | State |
| --- | --- |
| Boot | Stock Samsung S-Boot → our loader in the BOOT partition → XNU. Recovery (TWRP), EFS and the bootloaders are not touched |
| CPU | **All 8 cores**: 4 Cortex-A53 + 4 Mongoose (the Mongoose cluster needed the GIC distributor enabled before PSCI `CPU_ON`). Clocks through Samsung's APM firmware: A53 up to 1690 MHz, Mongoose up to 1703 MHz, a thermal guard, and a load governor with touch/GPU boost (`s8gov=1`) |
| Memory | **2 GiB** of the upper bank at `0x880000000` (of 4 GiB) |
| Storage | Own Exynos UFS driver. The iOS root is HFS+ in USERDATA (`sda24`), read/write |
| iOS userland | iPhone X 16.7.16 root filesystem and shared cache; launchd, daemons, backboardd, SpringBoard; daemons for missing hardware removed |
| **GPU** | **Mali-G71 MP20 renders the iOS UI**: our `S8Mali` XNU driver (jobs, MMU, IOSurface import, idle power) + Mesa 25.0.7 Panfrost built for iOS + an `EAGLContext` bridge in backboardd (`s8ca=1`). Lock screen, home screen, App Library, Cover Sheet, Settings, App Store and Safari drawn correctly |
| Display | 1125×2436 rendered, scaled to 1367×2960 by a kernel presenter thread at 60 Hz (CPU copy ~9 ms per frame) |
| GUI | Apple logo, lock screen, home screen, App Library, Control Center, Spotlight with keyboard, apps |
| Buttons | Power, volume up/down, Bixby (ALIVE GPIOs) → HID events |
| Clock | Date and time from the S2MPS17 RTC, read by the loader |
| Touch | S6SY761 on `hsi2c_23`, polled kernel driver → HID touch events |
| Battery | Real level, voltage and charger state from the MAX77865 |
| Testing without a person | Automatic return to TWRP (`s8autoreset`, early-panic reset, watchdog), kernel log ring in USERDATA, drop zone for test programs, scripted taps/swipes (`s8swipe`), screenshots (`s8shot`), per-process CPU (`s8top`) |

## Known bugs

- No Wi-Fi, Bluetooth, cellular, audio, cameras, sensors, GPS, vibration or SEP / Face ID.
- Photos aborts at launch. Settings sometimes crashes (CoreImage in the app wants OpenGL ES, which only backboardd has).
- The panel is fed by a CPU scale-and-copy; the hardware (DPP) path tears and is off.
- GPU driver: one shared address space, polled synchronous jobs, whole-surface cache flushes per frame. One unexplained backboardd crash seen once (c70).
- 2 of 4 GiB RAM used.
- Security is off: permissive AMFI/sandbox, stub AppleKeyStore, dyld insertion allowed. Do not put anything personal on it.

## Roadmap

Not continued after 2026-10-24. Done and open items:

- [x] S6SY761 touch driver in XNU
- [x] Fix the 4-core stalls (physical timer)
- [x] Mongoose cluster (all 8 cores) and full A53/Mongoose clocks with a thermal guard
- [x] 1125×2436 presentation, scaled to the panel height
- [x] RTC (date/time), battery (MAX77865)
- [x] 2 GiB RAM
- [x] GPU: Mali-G71 kernel driver, Mesa Panfrost on iOS, CoreAnimation drawing on the Mali
- [x] CPU load governor, idle CPU drain from missing-hardware daemons removed
- [ ] Tear-free zero-copy scanout through the DPP
- [ ] All 4 GiB of RAM (multi-range DRAM in pmap)
- [ ] GPU: per-process address spaces, job IRQs, tiler-heap growth, write-combined render targets
- [ ] Vibration motor, backlight brightness, audio, Wi-Fi (BCM4361)

Help is welcome: issues and pull requests.

## Installation

Step-by-step guide: **[docs/s8-install.md](docs/s8-install.md)**. Building on a Mac: **[docs/mac-build.md](docs/mac-build.md)** (loader and BOOT image native; the iOS root image in a Lima Linux VM).

In short:
1. An unlocked Exynos S8 (SM-G950F or SM-G950N) with TWRP and a full partition backup.
2. Fork this repository; the XNU kernel is built on GitHub Actions (workflow `xnu`, `MACHINE_CONFIG=A5XELTE`).
3. The iOS root image is built from the iPhone X 16.7.16 IPSW, which you download yourself, and is written to USERDATA.
4. `tools/dreamlte/make-boot.sh` builds the BOOT image (loader + XNU + Apple Device Tree), which is written to `sda7` from TWRP.

## How it boots

```
S-Boot (stock Samsung bootloader, EL1, MMU off)
  -> BOOT partition: shim/dreamlte (our loader instead of a Linux Image)
     -> unmasks the DECON trigger, logs to ramoops, wakes UFS
     -> copies XNU + ADT from the ramdisk to 0x880000000, builds boot_args
        -> XNU (xnu-8796, A5XELTE config + patch 0124-dreamlte-s8)
           -> UFS driver -> HFS+ root in USERDATA
              -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

## Layout

| Path | What it is |
| --- | --- |
| `shim/dreamlte/` | Loader for S-Boot: `loader.c`, UFS / SMP / touch probes |
| `xnu/patches/` | XNU patches; the S8-specific one is `0124-dreamlte-s8.patch` |
| `tools/dreamlte/apply-dreamlte.py` | Generator of patch 0124 |
| `tools/mkadt_dreamlte.py` | Apple Device Tree for the S8 |
| `tools/dreamlte/make-boot.sh` | Kernel → BOOT image |
| `tools/mkiosroot.sh`, `tools/dreamlte/finish-iosroot.sh` | iOS root image build |
| `tools/dreamlte/flash-chunks.ps1`, `flash-chunks.sh` | Writes an image to BOOT / USERDATA in verified chunks (PowerShell, bash for macOS/Linux) |
| `tools/dreamlte/pull-sweetlog.ps1`, `pull-sweetlog.sh` | Pulls the boot logs from TWRP |
| `tools/dreamlte/dreamlte_mali.cpp` | `S8Mali`, the Mali-G71 kernel driver (copied into XNU by apply-dreamlte.py) |
| `tools/dreamlte/mesa/` | Mesa 25.0.7 for iOS: build script, patches and the libdrm stand-in `s8drm` |
| `tools/dreamlte/ca/` | CoreAnimation bridge for backboardd (`libs8ca`, `libs8gl`: EAGLContext on Mesa, `s8swipe`) |
| `tools/dreamlte/cycle.sh`, `s8top.c`, `s8shot.c`, `s8drop.c` | Unattended test cycle, CPU profiler, screenshots, drop-zone runner |
| `docs/RESULTS.md` | **Summary of what this fork achieved and where it stopped** |
| `docs/plan.md`, `docs/spec.md` | Product plan (definition of done, work streams, milestones) and system specification |
| `DREAMLTE_PROGRESS.md` | **S8 port log**: hardware facts, every boot, open work (the handoff file) |
| `docs/history/` | Galaxy A5 2016 (`A5XELTE_PROGRESS.md`, `a5xelte-*.md`) and original SWEET / Redmi Note 10 Pro (`NOTES.md`, `upstream-README.md`) history, kept as reference |

## Authors

- [@Floppikrob](https://github.com/Floppikrob): Galaxy S8 port (original [ios-galaxy-s8](https://github.com/Floppikrob/ios-galaxy-s8))
- [@iamv4g](https://github.com/iamv4g): galaxios8 fork, continued work
- [@MakrSas](https://github.com/MakrSas): contributor, mainline Linux on the S8 ([LinuxS8](https://github.com/MakrSas/LinuxS8))

## Credits

- [orhunnx/ios-a5xelte](https://github.com/orhunnx/ios-a5xelte): the Galaxy A5 2016 port (4 KiB pages, Exynos, S-Boot) this project is based on
- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet): the original iOS 16 port to the Redmi Note 10 Pro (its README: [docs/history/upstream-README.md](docs/history/upstream-README.md))
- [MakrSas/LinuxS8](https://github.com/MakrSas/LinuxS8): mainline Linux on the Galaxy S8, the reference for touch, buttons and UFS
- Mainline Linux (Exynos 8895, HSI2C and S6SY761 drivers), [uniLoader](https://github.com/ivoszbg/uniLoader)
- Apple's open-source releases (xnu, IOHIDFamily, hfs), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), [apfs-fuse](https://github.com/sgan81/apfs-fuse)

iOS belongs to Apple and is not distributed here: download the IPSW yourself.

## License

The XNU patches are derived from XNU and fall under the [APSL 2.0](https://opensource.apple.com/apsl/). Code written after Linux drivers (for example `shim/dreamlte/touch_probe.c`) is GPL-2.0, see the SPDX headers. The upstream projects ios-sweet and ios-a5xelte did not state an overall license, so the remaining files have no separate license yet.
