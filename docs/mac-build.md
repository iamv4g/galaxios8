# Building on macOS (Apple Silicon)

The original guide ([s8-install.md](s8-install.md)) assumes Linux or WSL. Most of the pipeline also runs natively on macOS. This page says what was **tested on a Mac** and what was **not**. Nothing here needs the phone until step 5.

Tested on macOS 26 / Apple Silicon with Homebrew. Observations are separate from guesses.

## What runs where

| Piece | macOS native | State |
| --- | --- | --- |
| XNU kernel | GitHub Actions (macOS runner), or Xcode locally | CI run started on the fork; Mac side below |
| S8 loader (`shim/dreamlte`) | yes, `aarch64-elf` cross toolchain | **built on a Mac**, 58 752 bytes |
| ADT, payload, BOOT packer (Python) | yes, pure Python | ADT and BOOT repack **tested**; `mkpayload.py` needs a real kernel (see below) |
| iOS root image (HFS+ from the IPSW) | not ported yet | **not done**: `mkiosroot.sh` needs `apfs-fuse`, `mkfs.hfsplus` and a Linux `hfsplus` mount |
| Flash and log helpers | `flash-chunks.sh`, `pull-sweetlog.sh` | written, syntax-checked only; **never run against a phone** |

## 1. Tools

```bash
brew install aarch64-elf-gcc aarch64-elf-binutils coreutils gawk
```

`shim/dreamlte/build.sh` picks `aarch64-linux-gnu-*` if present (Linux) and falls back to `aarch64-elf-*`. The loader is freestanding, so the bare-metal toolchain is enough. Xcode's command line tools provide `python3`, `git` and `shasum`.

## 2. Backup first

Do this from TWRP, before anything is written (details in `DREAMLTE_PROGRESS.md`). Keep the files in the git-ignored `backup/`:

```bash
export ANDROID_SERIAL=<serial from: adb devices -l>   # the S8 only; never a phone with a different SoC
adb exec-out "dd if=/dev/block/sda7 bs=1048576" > backup/BOOT.img
adb exec-out "dd if=/dev/block/sda3 bs=1048576" > backup/EFS.img
adb exec-out "dd if=/dev/block/sda8 bs=1048576" > backup/RECOVERY.img
shasum -a 256 backup/*.img   # compare with: adb shell sha256sum /dev/block/sdaN
```

TWRP has no `/dev/block/by-name`; use the `sdaN` names. `/sdcard` lives on USERDATA (`sda24`) and is erased by the root write, so never keep backups there. When several devices are attached, always set `ANDROID_SERIAL` (or `adb -s`).

## 3. Kernel

Run the `xnu` workflow in your fork (Actions → xnu → Run workflow, `machine_config=A5XELTE`), or:

```bash
gh workflow run xnu -R <you>/galaxios8 --ref main -f machine_config=A5XELTE
gh run list -R <you>/galaxios8 --workflow xnu --limit 1
gh run download <RUN_ID> -R <you>/galaxios8 -n xnu-kernel -D xnu/out
```

This gives `xnu/out/kernel.development.a5xelte`.

## 4. Loader and BOOT image (no phone needed)

```bash
bash tools/dreamlte/make-boot.sh xnu/out/kernel.development.a5xelte backup/BOOT.img s8-ios-boot.img \
  debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=USERDATA \
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 \
  launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0 a5fb_noconsole=1 sweetfb_native=1
python3 tools/dreamlte_bootimg_info.py s8-ios-boot.img
```

Checked on the Mac without a kernel: `build.sh` alone builds `shim/dreamlte/out/Image`, and repacking the stock `BOOT.img` with a dummy payload gives a valid image that `dreamlte_bootimg_info.py` parses (kernel = our loader, ramdisk = payload, stock DTBH kept). `build.sh` changes into its own directory, so pass **absolute paths** to it; `make-boot.sh` resolves them itself.

Notes:

- `tools/mkadt_dreamlte.py` warns `d22ap.adt missing, no /product node` until you extract the iPhone X device tree (needs the IPSW and `ipsw`, see below). The BOOT image still builds, but boot with the product node for the real iOS run.
- The loader is deterministic only for a given compiler. CI (`loader` workflow, run 39650850106): on Ubuntu the pre-macOS `build.sh` and the current one give a byte-identical image (58 816 bytes). The Mac `aarch64-elf-gcc` 16.2 image is 58 752 bytes, so Mac and Linux loaders are **not** byte-identical; whether the 64-byte difference matters on the phone is untested.

## 5. Writing to the phone (needs the S8, destructive)

Only BOOT `sda7` and later USERDATA `sda24` are written, from TWRP. Check the model first: `adb shell getprop ro.product.device` must print `dreamlte`.

```bash
export ANDROID_SERIAL=<S8 serial>
adb push s8-ios-boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/sda7 bs=1048576 conv=fsync; sync"
adb reboot
```

For the root image, after building it:

```bash
python3 tools/dreamlte/chunk-manifest.py ios16-root.img ios16-root.json
bash tools/dreamlte/flash-chunks.sh ios16-root.img ios16-root.json USERDATA
```

`flash-chunks.sh` asks you to type the partition name, refuses anything but BOOT or USERDATA, requires TWRP and `dreamlte`, and verifies each 32 MiB chunk by SHA-256. Read the logs afterwards with `bash tools/dreamlte/pull-sweetlog.sh boot32` (read-only on the phone).

## Not done yet: the iOS root image

You download the iPhone X 16.7.16 (20H392) IPSW yourself. `tools/mkiosroot.sh` and `finish-iosroot.sh` still target Linux. A macOS version is plausible with native tools (`hdiutil attach` for the APFS images, `newfs_hfs -s -J` for a case-sensitive journaled HFS+ image, `ditto` or Homebrew `rsync` to copy) but is untested; open questions are ownership and hard links, extended attributes, and what macOS adds to the volume (Spotlight, `.fseventsd`). Until that exists, the root image still needs the Linux path in [s8-install.md](s8-install.md).
