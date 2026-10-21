# Installing iOS on the Galaxy S8 (SM-G950F / SM-G950N)

> ⚠️ **Risks.** This is an experimental build. Installing it **erases USERDATA** (all Android data). It needs an unlocked bootloader (Knox trips). Only two partitions are written: **BOOT (`sda7`)** and **USERDATA (`sda24`)**. EFS, recovery and the bootloaders are not touched, but **make a full backup** before you start.
>
> Exynos 8895 (`dreamlte`) only: **SM-G950F** (upstream) and **SM-G950N** (tested in the galaxios8 fork; same BOOT `sda7` and USERDATA `sda24`). Other Exynos variants are untested. The Snapdragon models (G950U/U1/W/0/SC) and the S8+ (G955x) are not supported: the S8+ has a different panel and partition numbers may differ. Android props can lie on ported ROMs; check `/proc/device-tree/model` (`DREAMLTE`) and the bootloader version.

## 0. Requirements

- Galaxy S8 SM-G950F or SM-G950N, OEM unlock, **TWRP** installed (TWRP has no `/dev/block/by-name`: use the `sdaN` names).
- macOS instead of Linux: see [mac-build.md](mac-build.md).
- PC: Linux, or Windows 10/11 with **WSL Ubuntu**. Shell commands below are for Linux/WSL; the `.ps1` scripts are for PowerShell on Windows.
- `adb` (platform-tools), `python3`, `gh` (GitHub CLI).
- On Linux/WSL: `gcc-aarch64-linux-gnu`, `hfsprogs` (`mkfs.hfsplus`, `fsck.hfsplus`), `rsync`, [`apfs-fuse`](https://github.com/sgan81/apfs-fuse) (build from source), the `hfsplus` kernel module (on WSL, build it for the WSL kernel and load it with `insmod`), [`ipsw`](https://github.com/blacktop/ipsw).
- About 40 GB of free space.

## 1. Backup

Boot into TWRP, select Boot, EFS, System and Data under "Backup" and save it to the SD card or the PC. Also keep a raw copy of BOOT:

```bash
adb shell "dd if=/dev/block/sda7 of=/sdcard/BOOT.img"
adb pull /sdcard/BOOT.img backup/BOOT.img
```

`backup/BOOT.img` (the stock BOOT) is needed later: it supplies the image header and the DTBH.

## 2. XNU kernel

The kernel is built on GitHub Actions macOS runners (free for public repositories).

1. Fork this repository (`iamv4g/galaxios8`).
2. Actions → **xnu** → *Run workflow* (`machine_config` = `A5XELTE`, the default). A build also starts on every change to `xnu/patches/**`.
3. Download the artifact:

```bash
gh run list -R YOU/REPO --workflow xnu --limit 1
gh run download RUN_ID -R YOU/REPO -n xnu-kernel -D xnu/out
```

This gives `xnu/out/kernel.development.a5xelte`. The machine config is named A5XELTE; the S8 support comes from patch `0124-dreamlte-s8.patch`.

## 3. iOS root

1. Download the **iPhone X (iPhone10,3 / iPhone10,6) iOS 16.7.16 (20H392)** IPSW from Apple's servers yourself (ipsw.me lists the links). It is not distributed here.
2. Unpack the IPSW (it is a zip). The names of the OS, SystemOS and AppOS images are in `BuildManifest.plist`.
3. The iPhone X device tree, which MobileGestalt needs:

```bash
ipsw img4 im4p extract Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

4. Build the image (16 GB, HFS+) as root:

```bash
sudo tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo tools/dreamlte/finish-iosroot.sh ios16-root.img
```

`finish-iosroot.sh` creates the log directories, applies `tools/sweet-root-tweaks.py` (skips Setup, seeds the activation state, disables watchdogs, and so on) and checks the filesystem.

## 4. Write the root to USERDATA

With the phone in TWRP. The image is written in 32 MiB chunks, each verified by SHA-256. After a USB drop, just run the command again.

```bash
python3 tools/dreamlte/chunk-manifest.py ios16-root.img ios16-root.json
```

In PowerShell:

```powershell
tools\dreamlte\flash-chunks.ps1 -Image \\wsl.localhost\Ubuntu\root\ios16-root.img -Manifest ios16-root.json -Partition USERDATA
```

The kernel uses the last 4 MiB of USERDATA as its log ring; a 16 GB image does not reach it.

## 5. BOOT image

```bash
bash tools/dreamlte/make-boot.sh xnu/out/kernel.development.a5xelte backup/BOOT.img s8-ios-boot.img \
  debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=USERDATA \
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 \
  launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0 a5fb_noconsole=1 sweetfb_native=1
```

The script builds the ADT (`tools/mkadt_dreamlte.py`) and the payload (`tools/mkpayload.py`), compiles the `shim/dreamlte` loader and packs BOOT with the stock header.

Flash it (phone in TWRP):

```bash
adb push s8-ios-boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/sda7 bs=1048576 conv=fsync; sync"
adb reboot
```

First check that `sda7` really is BOOT: `adb shell ls -l /dev/block/by-name/BOOT` (the path may differ).

## 6. Booting

- The loader's text comes first, then the Apple logo. Reaching the lock screen takes **about 15 minutes** (CPU rendering).
- `cpus=4` is the normal setting since the physical-timer fix (boot 25). `cpus=1` is a fallback for debugging; the Mongoose cores cannot be started yet, so do not go above 4.

### Back to TWRP

Hold **Power + Volume Down** (reboot); as soon as the screen goes dark, hold **Power + Bixby + Volume Up**. Be quick: if iOS starts booting again first, it begins a new kernel log ring and the previous boot's log is lost.

### Logs

In TWRP:

```powershell
tools\dreamlte\pull-sweetlog.ps1 boot1
```

This saves `logs/sweetlog-boot1.txt` (the kernel console from disk) and `logs/pmsg-boot1.txt` (the loader and the early kernel). Attach both to issues.

## Back to Android

Restore BOOT and Data from the TWRP backup (or `dd` the saved `BOOT.img` back to `sda7`), then run "Format Data" in TWRP.

## Boot arguments

| Argument | Purpose |
| --- | --- |
| `cpus=N` | How many A53 cores to start (1–4) |
| `rd=ufsroot0 ufsroot=USERDATA` | Root is the HFS+ volume in USERDATA |
| `ufsrw=1 rootrw=1` | Root mounted read/write |
| `sweetfb_native=1` | Present the full 1440×2960 panel to iOS. `0` gives the faster centered 1125×2436 area |
| `a5fb_keep_on=1` | Keep the panel on; leave it out so that locking the phone blanks the screen |
| `sweetfb_hz=N` | Limit the CPU copy to the panel to N frames per second |
| `a5fb_noconsole=1` | Do not draw the kernel console over iOS |
| `a5touchprobe=0` | Disable the Galaxy A5 touch probe (it aborts on the S8) |
| `wdt=-1 wdt_test=no_user_checkin` | No watchdogs (the system is slow) |
| `ignition_level=1 launchd_unsecure_cache=1` | Load the shared cache from the cryptex without signature checks |
