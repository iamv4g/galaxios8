# Building on macOS (Apple Silicon)

The original guide ([s8-install.md](s8-install.md)) assumes Linux or WSL. Most of the pipeline also runs natively on macOS. This page says what was **tested on a Mac** and what was **not**. Nothing here needs the phone until step 5.

Tested on macOS 26 / Apple Silicon with Homebrew. Observations are separate from guesses.

## What runs where

| Piece | macOS native | State |
| --- | --- | --- |
| XNU kernel | GitHub Actions (macOS runner), or Xcode locally | CI run started on the fork; Mac side below |
| S8 loader (`shim/dreamlte`) | yes, `aarch64-elf` cross toolchain | **built on a Mac**, 58 752 bytes |
| ADT, payload, BOOT packer (Python) | yes, pure Python | ADT and BOOT repack **tested**; `mkpayload.py` needs a real kernel (see below) |
| iOS root image (HFS+ from the IPSW) | in a Lima Linux VM on the Mac | **built and written to the phone** (see below); not native macOS |
| Flash and log helpers | `flash-chunks.sh`, `pull-sweetlog.sh` | **used on the phone**: USERDATA written 512/512 verified, logs pulled |

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

## iOS root image on a Mac (Lima VM)

macOS cannot run `mkiosroot.sh` (it needs `apfs-fuse`, `mkfs.hfsplus` and a Linux `hfsplus` mount, all as root). A small Linux VM does it without any `sudo` on the Mac. **Done this way on 2026-10-22; the image booted is described in `DREAMLTE_PROGRESS.md`.**

```bash
brew install lima
limactl start --name=s8 --cpus=6 --memory=12 --disk=120 --vm-type=vz --tty=false template:ubuntu-24.04
limactl shell s8 -- bash -lc '
  sudo apt-get update
  sudo apt-get install -y linux-modules-extra-$(uname -r) hfsprogs rsync git cmake g++ \
       libfuse3-dev fuse3 libbz2-dev zlib1g-dev libattr1-dev unzip python3
  sudo modprobe hfsplus
  git clone --recursive https://github.com/sgan81/apfs-fuse.git ~/apfs-fuse
  cd ~/apfs-fuse && mkdir -p build && cd build && cmake .. && make -j6
  sudo install -m 0755 apfs-fuse /usr/local/bin/apfs-fuse'
```

The VM sees your Mac home read-only, so it reads the IPSW and the repo in place. Names of the OS / SystemOS / AppOS images come from `BuildManifest.plist` (for 20H392: `087-86698-044.dmg`, `087-86593-047.dmg`, `087-86715-047.dmg`).

```bash
R=$HOME/lab/ios-galaxy-s8          # this repo on the Mac
limactl shell s8 -- bash -lc "
  mkdir -p ~/s8 && cd ~/s8
  unzip -o $R/research/ipsw/<IPSW> 087-86698-044.dmg 087-86593-047.dmg 087-86715-047.dmg
  sudo bash $R/tools/mkiosroot.sh \$PWD/087-86698-044.dmg \$PWD/087-86593-047.dmg \$PWD/087-86715-047.dmg \$PWD/ios16-root.img 16G
  sudo bash $R/tools/dreamlte/finish-iosroot.sh \$PWD/ios16-root.img
  sudo chown \$(id -u) ios16-root.img
  python3 $R/tools/dreamlte/chunk-manifest.py ios16-root.img ios16-root.json"
mkdir -p research/s8root
limactl cp s8:/home/$USER.guest/s8/ios16-root.img s8:/home/$USER.guest/s8/ios16-root.json research/s8root/
```

The iPhone X device tree for the ADT `/product` node is extracted on the Mac:

```bash
brew install blacktop/tap/ipsw
unzip -j research/ipsw/<IPSW> Firmware/all_flash/DeviceTree.d22ap.im4p -d research/ipsw-x/
ipsw img4 im4p extract research/ipsw-x/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

Then rebuild the BOOT image (step 4) so the ADT carries `/product`, and flash both (step 5). `flash-chunks.sh` needs the fix from commit `ef2a987`: the first version wrote only the first chunk.
