# GPU acceleration for iOS on the Galaxy S8: research notes

Date: 2026-10-23. Scope: can iOS 16.7.16 (20H392, iPhone X / D22 root) on the Galaxy S8 draw with the
Exynos 8895's Mali-G71 MP20 instead of the CPU, and what should be tried first. No code was changed and
nothing ran on the phone. Labels used below:

- **[fact]** checked here, with the source given (our own dyld cache and kernelcache from the 20H392 IPSW,
  Mesa, Linux and Samsung sources, primary web pages).
- **[reported]** a claim made by another project that I did not reproduce.
- **[estimate]** my own judgement.

## Summary

1. **There is a workable path, and it is not Metal.** CoreAnimation's render server in iOS 16 has three
   backends: Metal, then OpenGL ES (`EAGLContext` with API 3 plus `CA::OGL::GLESContext`), then its own
   software rasterizer. Our phone falls through to the last one [fact: the backboardd samples in
   `DREAMLTE_PROGRESS.md` show `CA::OGL::SW::*`]. HackiOS (below) reports CoreAnimation drawing through the
   OpenGL ES backend on an Adreno GPU, using Mesa behind a replacement `EAGLContext` that it injects into
   backboardd [reported]. The same design fits Mali-G71: Mesa Panfrost behind the `EAGLContext` bridge, and a
   small Panfrost-style GPU driver in XNU.
2. **Mali-G71 is a better kernel target than Adreno was for HackiOS.** It is a Job Manager GPU with no
   firmware, no signed "zap shader" and no TrustZone step [fact: JM vs CSF split in Mesa and Linux]. Its
   registers, three IRQs, power domain and DVFS id are all in Samsung's device tree. Its clock is an ACPM
   DVFS domain, `0x0B040004`, which uses the same mechanism this project already uses for the CPU clusters
   [fact]. Linux's panfrost driver supports G71 (`GPU_MODEL(g71, 0x6000, …)`) [fact].
3. **Mesa's G71 support is real but the least tested.** G71 has been in Panfrost's model table for years.
   The Mesa docs listed it as "not yet supported" until 2026-10-06, when the line was changed to supported
   with the note "As far as I can tell, G71 has been supported for a long time" [fact]. It is not
   conformant, and early silicon has hardware errata. One extra constraint: HackiOS's bridge uses OSMesa,
   which Mesa removed in 25.1. So it is either Mesa 25.0.x or porting another way to get a context [fact].
4. **Drop AGX emulation.** That means a kernel-side translation of Apple's AGX command stream and G10
   shader ISA into Mali. It would take years and has no partial payoff [estimate].
5. **Metal for apps comes later and is optional.** HackiOS has a Metal plugin bundle and an AIR→NIR shader
   frontend that is still in progress (293 of 456 system shaders run, per their README) [reported]. Their
   backend glue is tied to freedreno/ir3.
6. **First milestone, a few weeks [estimate]: "the Mali does one job".** A diagnostic loader build (like
   `MNGS_DIAG`) or a small XNU probe does the following: switch on `pd-g3d`, ask ACPM for 260 MHz, read
   `GPU_ID`, power up L2 and the shader cores, set up one MMU address space, and run a single *Write value*
   job that stores a known word in RAM. While that waits for the phone, cross-build Mesa 25.0.7
   (`softpipe,panfrost`, OSMesa) for iOS on the Mac.

## 0. Where the project is today (from `DREAMLTE_PROGRESS.md`)

- backboardd renders with CoreAnimation's software renderer (`CA::OGL::SW::image_sampler`,
  `scan_convert`). The kernel's SweetFramebuffer (an IOMobileFramebuffer/AppleCLCD stand-in) takes each
  swap's IOSurface. It snapshots it (~5 ms), and a presenter thread scales 1125x2436 to 1367x2960 on the
  CPU (~14 ms) into the DECON framebuffer at `0xcc000000`.
- p8/p9: ~50 frames/s rendered while swiping, the panel at ~42/s, CPU saturated. CPU clocks have since
  been raised through ACPM (commit ee605ef).
- `docs/plan.md` lists GPU acceleration as out of scope ("Mali-G71 cannot run Apple's Metal/AGX
  stack"). That is true for Apple's own stack. It is not true for CoreAnimation's OpenGL ES backend. See
  below.

## 1. How iOS 16 uses the GPU

### 1.1 Userspace pieces (iPhone X, 20H392) [fact: our dyld cache, `ipsw dyld str/info/objc`]

| Piece | Version / evidence | Role |
| --- | --- | --- |
| `Metal.framework` | 306.6.6 | API; `MTLCreateSystemDefaultDevice`; finds the driver bundle; talks to `com.apple.MTLCompilerService` (XPC) |
| `IOGPU.framework` | 65.60.1 | Shared driver base classes: `IOGPUMetalResource`, `IOGPUMetalTexture`, `IOGPUMetalHeap`, `IOGPUMetalResourcePool`, … |
| `AGXMetalA11.bundle` (`/System/Library/Extensions`) | 227.6.5 | The A11 Metal driver: `AGXA11FamilyCommandBuffer`, `…RenderPipeline`, `…Texture`, `…ComputeContext`, `…BlitContext`, … built on the IOGPU classes |
| `GPUCompiler.framework` (`libGPUCompilerImpl`) | 31001.722.3.1 | AIR (Metal's LLVM IR) → AGX machine code, run by MTLCompilerService |
| `AppleMetalGLRenderer.bundle`, `OpenGLES.framework` (`GLEngine.bundle`, `libGLProgrammability`, …) | present | OpenGL ES. The bundle name suggests GLES is layered on Metal on A11; I infer this from the name and did not check it |
| `QuartzCore.framework` | 1045.17.2 | CoreAnimation, including the render server that runs inside backboardd |
| `IOAccelerator.framework`, `IOSurface`, `IOMobileFramebuffer` | present | Service lookup, shared surfaces, display |

How Metal finds its driver [fact]. The kernel's AGX personality carries `MetalPluginName = AGXMetalA11`,
`MetalPluginClassName = AGXA11Device` and `MetalStatisticsName = AGXMetalStatisticsExternalA11` (prelink
info in the 20H392 kernelcache). Metal and IOGPU contain the strings `MetalPluginName` and
`MetalPluginClassName`. `AGXGPURawCounter` has an assertion string that shows the lookup:
`IORegistryEntryCreateCFProperty(acceleratorPort, CFSTR("MetalPluginName"), …)`. So any IOAccelerator-class
service that publishes those properties gets its bundle loaded by Metal. HackiOS ships a
`metal_plugin.plist` (principal class `RedwoodMetalDevice`) and reports that this works on 16.5 [reported].

### 1.2 Kernel pieces [fact: `ipsw kernel kexts` and class strings in the 20H392 kernelcache]

- `com.apple.iokit.IOGPUFamily` 65.60.1: `IOGPUDevice`, `IOGPUDeviceUserClient`, `IOGPUCommandQueue`,
  `IOGPUResource`, `IOGPUMemoryMap`, `IOGPUSysMemory`, `IOGPUEventMachine`, `IOGPUNotificationQueue`,
  `IOGPUDeviceShmem`, `IOGPUSegmentResourceList`, `IOGPUMTLEvent`/`Fence`/`SharedEventFence`.
- `com.apple.AGXG10P`: `AGXAcceleratorG10P_A0/_B0`, `AGXDeviceUserClient`, `AGXCommandQueue`, channels
  `AGX3DChannelG10`, `AGXTAChannelG10`, `AGXCLChannelG10`, `AGXGartG10`, parameter-buffer management,
  `AGXFirmware`, `AGXArmFirmwareKingfisherV4`.
- `com.apple.AGXFirmwareKextG10PRTBuddy` / `AGXFirmwareKextRTBuddy` 227.6.5: the GPU's RTKit firmware
  coprocessor. The A11 GPU, like M1, is driven through firmware.
- `com.apple.iokit.IOSurface`, `AppleMobileDispH10P` (A11's in-kernel IOMobileFramebuffer),
  `AppleM2ScalerCSCDriver` (the scaler / colour-converter behind IOSurfaceAccelerator), `AppleT8015DART`.

### 1.3 The userspace–kernel interface

- Metal's driver bundle talks to IOGPUFamily/AGX through IOKit user-client calls. Memory is allocated by
  selector and mapped into both the process and the GPU. Command buffers and descriptors live in shared
  memory and are handed over with a submit selector, along with a resource/mapping list [fact for the
  M1/macOS equivalent: Rosenzweig, "Dissecting the Apple M1 GPU, part II": "I interface with the hardware
  via the IOKit kernel driver in an identical fashion to the system's Metal userspace driver"; "this
  mapping table, along with the root command buffer, is allocated via a special IOKit selector"]. The A11
  selectors and structures are undocumented. I assume they are similar in kind [estimate].
- Below that, the kernel driver does not touch the GPU directly. It talks to the GPU firmware through
  shared-memory structures: "almost 1000 fields", "more than 100 data structures", and "Apple doesn't keep
  the firmware structure definitions stable from version to version" [fact for M1: Asahi Lina, "Tales of
  the M1 GPU"]. Asahi's work covers G13 and later. Nobody has documented A11's G10.

### 1.4 What CoreAnimation does, and why we get software today

The following comes from HackiOS's reverse-engineering notes for 16.5 (`userland/src/inject.c`, `eagl.c`)
[reported], and it matches what we see:

- The render server lives in `CA::WindowServer::EAGLServer`. Its `renderer()` first calls
  `CAMetalContextCreate()`, a `dispatch_once` around `MTLCreateSystemDefaultDevice()`. A non-nil device
  leads to `CA::OGL::new_metal_context()`, which loads `QuartzCore.framework/default.metallib`.
- If the device is nil, it tries `[EAGLContext initWithAPI:3]` and `CA::OGL::GLESContext`. If that fails,
  it uses `CA::WindowServer::Server::sw_renderer()`.
- On our phone there is no IOAccelerator, so Metal is nil. The stock OpenGL ES also needs Apple's GPU
  driver, so it fails too, and we get the software renderer [fact for the outcome; the exact reason the
  stock GLES fails is my inference].
- The GLES backend still exists in 20H392 [fact, partly]. QuartzCore references the selectors
  `initWithAPI:properties:` and `attachImage:toCoreSurface:invertedRender:` and has a `CA_ENABLE_OGL`
  environment string. The vtable and shader-table addresses must be found again for 20H392. HackiOS's
  addresses are for 16.5 (20F66).
- The GLES backend is a legacy path. CoreAnimation draws layers with about 90 fragment "functions". The
  GLES renderer has shaders for fewer than half of them, while the Metal renderer has all of them. Missing
  functions are either not drawn or drawn "with no program". HackiOS adds stand-ins (blurs, vibrancy
  groups, swizzles such as BGRA in `GL_RGBA`, pixel-coordinate samplers) [reported]. Expect visual gaps
  that need work.
- `EAGLContext` only needs a handful of methods for CoreAnimation: `-initWithAPI:properties:`, `-API`,
  `-setParameter:to:`, `-getMacroContextPrivate` (a GL dispatch table at fixed slots), `-texImageIOSurface:…`
  and `-attachImage:toCoreSurface:invertedRender:` [reported].

### 1.5 Display-side composition (IOMFB layers)

On real iPhones, CoreAnimation can hand some layers straight to the display controller as IOMFB "source
layers" or "detached" layers. These are typically video or full-screen content that skips the composite.
QuartzCore has strings such as `"Display %u setting IOMFB source layer %u colorspace…"`, `detached-content`,
`detached-dirty` and `IOMobileFramebufferSwap*` [fact: our cache]. To use this we would have to implement
the multi-layer swap API in SweetFramebuffer and map layers onto DECON windows/DPP channels. Even then,
ordinary UI is still composed by the renderer, so this does not remove the CPU rendering cost [estimate].

## 2. Prior art

### 2.1 HackiOS (oosexgang): the central reference

Repositories: `oosexgang/hackios-xnu-kernel` (APSL-2.0 for the XNU parts) and `oosexgang/hackios-xnu-userland`
(MIT). The organisation was created 2026-10-16 and the repositories on 2026-10-22. I read them as data and
ran nothing. They boot XNU `xnu-8796.121.2` with unmodified iOS 16.5 user space on a POCO X5 Pro
(Snapdragon 778G, Adreno 642L) and in QEMU, and report "Adreno 642L GPU through Mesa/freedreno (`gpu=1`,
OpenGL ES for CoreAnimation)", with photos from the phone [reported]. I found no frame-rate figures.

How it works, from their sources:

- **Kernel GPU driver** (`xnu/files/bsd/dev/redwood_adreno.c`, 183 KB): a step-by-step bring-up of the
  Adreno. Steps cover the power switch, clocks, the GMU, the SMMU, the GMU firmware and HFI queues,
  TrustZone authenticating the zap shader, the SQE microcode, the ring, and leaving secure mode. The
  register layouts come from Linux `drm/msm`. Each step can be run on its own through boot-args, which
  helped them survive resets caused by the hypervisor. Most of that complexity is Qualcomm-specific.
- **Kernel UAPI** (`xnu/files/iokit/Kernel/RedwoodGPU.cpp`, 48 KB): an IOService whose user client speaks
  Linux's **msm DRM** interface. The DRM request number is the selector and the struct goes in and out
  unchanged (`GET_PARAM`, `GEM_NEW`, `GEM_INFO`, `GEM_SUBMIT`, `WAIT_FENCE`, …). Buffer mmap is done as
  client memory, and one extra request imports an **IOSurface** as a GPU buffer for zero-copy rendering.
  It is single address space, waits by polling (no IRQs yet), and recovers from hangs by reset.
- **Mesa** (`tools/mk-mesa.sh`): Mesa **25.0.7**, gallium `softpipe,freedreno`, `-Dosmesa=true`, no LLVM, no
  EGL/GLX, cross-built with the iOS 16.5 SDK into one `libOSMesa.8.dylib`. `tools/mesa/redwood-drm/` is a
  roughly 200-line libdrm stand-in that turns DRM ioctls into `IOConnectCallStructMethod`.
  `freedreno-darwin.patch` (12 KB) makes Mesa build without DRM. Smaller ir3 patches fix compiler issues.
- **The OpenGL ES bridge** (`userland/src/eagl.c`, 169 KB, plus `mkeagl.py` and `eagl_slots.txt`): a
  replacement for the six `EAGLContext` methods that CoreAnimation uses, backed by an OSMesa ES context
  (`gl_osmesa.c`). It generates the dispatch table at the slot offsets of iOS's own OpenGLES. IOSurface
  textures share memory with the surface where the driver allows it and are copied otherwise. Shader text
  is fixed up for CoreAnimation's dialect, and stand-ins replace missing CA functions. It also carries an
  extensive tracing system.
- **Injection** (`userland/src/inject.c`, 55 KB): loaded into backboardd through `DYLD_INSERT_LIBRARIES`,
  which "dyld honours … because the kernel's AMFI stand-in says so". It swizzles
  `-[CAWindowServer _detectDisplays]` to empty Metal's device array, so the server keeps to GLES even when
  their Metal plugin is present. It patches vtable entries such as `EAGLServer::renderer` and
  `GLESContext` function queries, and adds shader sources to CoreAnimation's GLES shader table. Every
  address is checked against the expected 16.5 contents before it is written.
- **Metal** (`redwoodmetal.m`, 269 KB, `metal_plugin.plist`, `tools/mesa/air/*`, `docs/air.md`): a Metal
  plugin bundle and an AIR bitcode→NIR frontend. The corpus report says 321 of 456 system shader modules
  compile through ir3 and none of the 135 "visible function" modules do. The README says 293 run
  [reported]. CoreAnimation is deliberately kept off Metal.

What carries over to the S8: the whole userspace design (bridge, injection, Mesa cross-build, UAPI style)
is GPU-agnostic except for the Mesa driver choice. What does not carry over: the Adreno bring-up, the msm
UAPI, the 16.5 addresses, their IOSurface/IOMFB stand-ins (we have our own, patches 0046/0068/0109/0124)
and the ir3-specific AIR glue.

### 2.2 Emulators and Corellium

- eShard, "Emulating iOS 14 with QEMU": "every graphical rendering ends up going through their Metal
  API". Emulating the Apple GPU "would be way too complex". The old `gpu=0` boot-arg is gone in iOS 14.
  Patching QuartzCore to its software fallback worked "much slower, and had artifacts". Forwarding Metal
  calls to a real device was shelved [fact].
- `TrungNguyen1909/qemu-t8030` is archived (last push 2022-10-22). Forks such as `ChefKissInc/QEMUAppleSilicon`
  are active. As far as I found, all of them use software rendering. None emulates AGX [fact for the repo
  state; partly secondhand for the rendering].
- Corellium: I found no primary statement of GPU or Metal support for virtual iPhones. Their public
  getting-started page talks about OpenGL ES pass-through, apparently for Android [fact: what I could
  find; unknown otherwise].

### 2.3 Asahi Linux (AGX)

Asahi documents the M1/M2 firmware interface and has a Rust kernel driver and Mesa drivers (`asahi`,
`honeykrisp`). It confirms how heavy Apple's GPU stack is: firmware-mediated, version-unstable,
undocumented. That supports dropping option A. It does not help with A11/G10 [fact/estimate].

### 2.4 Darling Indium

`darlinghq/indium`: Metal on Vulkan 1.3, with "Iridium" translating AIR to SPIR-V. It is an internal
library, not a drop-in Metal. Last push was 2023-04 [fact]. Only relevant if a Metal-on-PanVK route is
ever tried.

### 2.5 Panfrost, PanVK and Mali-G71 specifically [fact unless marked]

- Mesa `main` docs list "G71, G72 — Bifrost (v6) — GLES 3.1, GL 3.1, Vulkan 1.3". Commit `5eab85a5`
  (2026-10-06, Erik Faye-Lund) changed the docs to list G71 as supported. Mesa **25.0.7's** docs still say
  "Other Midgard and Bifrost chips (e.g. G71) are not yet supported", yet its model table already has
  `MODEL(0x6000, 0, "G71", "TMIx", NO_ANISO, 8192, {})`. Conformance claims cover G52, G57, G610 and later,
  not G71.
- Linux panfrost: `GPU_MODEL(g71, 0x6000, GPU_REV_EXT(g71, 0, 0, 1, _05dev0))`. G71 errata
  `HW_ISSUE_TMIX_8463` and `TMIX_8438`, and r0p0-05dev0 adds `T76X_3953`. There is a G71-only JM_CONFIG
  coherency quirk. Bifrost uses AArch64-format 4 KiB GPU page tables (`AS_TRANSCFG_ADRMODE_AARCH64_4K`).
  This matches our 4 KiB XNU pages.
- PanVK in 25.0.7 is built for arch 6, 7 and 10. Bifrost features are limited (`arch >= 10` gates
  multiview, minmax and others). It is experimental on some GPUs and needs
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
- Gallium panfrost's kernel needs are small. The `pan_kmod` panfrost backend (497 lines) uses
  `CREATE_BO`, `MMAP_BO`, `GET_BO_OFFSET`, `GET_PARAM`, `WAIT_BO` and `MADVISE`. The gallium driver adds
  `SUBMIT` plus DRM syncobjs (create/wait/destroy, sync-file import/export). That is about the size of
  HackiOS's msm subset. `pan_kmod` also gives a cleaner place for a Darwin backend than a fake libdrm.
- OSMesa (HackiOS's way to get a context) exists in 25.0.7 and is gone in 25.1.0 and `main`.

### 2.6 The S8's GPU hardware [fact: Samsung `exynos8895.dtsi`, branch lineage-19.1, and our ECT dump]

- `mali@13900000`, `compatible = "arm,mali"`, `reg = <0x13900000 0x5000>`. IRQs SPI 60 (JOB), 61 (MMU),
  59 (GPU), so GIC INTIDs 92, 93, 91.
- Power domain `pd_g3d` at PMU `0x164840e0` (`cal_id 0xB1380008`). `gpu_pmu_status_reg_offset 0x40E4`.
- DVFS: `g3d_cmu_cal_id = ACPM_DVFS_G3D = 0x0B040004`. That is the same ACPM vclk family as
  `ACPM_DVFS_CPUCL0/1` (`0x0B040002/3`), which this project already drives with `FREQ_REQ`. The table runs
  260–839 MHz, and Samsung caps it at `gpu_max_clock = 546000` with a thermal step-down table. The GPU TMU
  is `tmuctrl_2` at `0x10084000`, id 2. Our ECT shows `dvfs_g3d` with nine levels and ASV group G3D:4.
- Samsung's kernel driver is Arm's kbase (`drivers/gpu/arm/b_r16p0`, plus `tMIx/r0p0`) with
  `platform/exynos/gpu_control.c` and related files. That code is the reference for the power and clock
  sequence. The kbase UAPI itself is useless to us because Mesa does not speak it.
- There is also a FIMG2D 2D blitter at `0x15100000`, behind SysMMUs `0x13A60000/0x13A70000`, and the DPP
  channels already noted in the plan.

## 3. Options, ranked by feasibility

| # | Option | Feasibility | Effort to first useful result [estimate] | Verdict |
| --- | --- | --- | --- | --- |
| A | **OpenGL ES bridge + Mesa Panfrost + XNU Mali driver** (the HackiOS design on Mali) | Good: every piece exists in open source, and one project reports it working on another GPU | 3–5 months to CoreAnimation on the GPU; weeks to first signs of life | **Primary path** |
| B | Lighter wins: DPP or G2D scaling for the presenter, zero-copy scan-out, IOMFB multi-layer swaps | Good | 1–4 weeks each | Do in parallel; they remain useful with a GPU |
| C | Own Metal plugin (MetalPluginName bundle) on Panfrost via AIR→NIR→Bifrost | Plausible (HackiOS shows the plugin loads), very large | 6+ months for useful app coverage | Later phase, only after A |
| D | AGX-compatible kernel driver translating Apple's command stream and G10 ISA to Mali | Not realistic | Years; undocumented firmware ABI and ISA; no partial payoff | **Drop** |
| E | Metal on PanVK via an Indium-style layer | Poor: PanVK Bifrost is experimental and Indium is dormant | Large | Drop for now |

### A in detail: what porting HackiOS's approach to Mali-G71 needs

1. **XNU Mali driver (new code, about 3–6 k lines [estimate]).** Linux panfrost is about 219 KB of source.
   Most of it is DRM scaffolding (GEM shmem, drm_sched, devfreq, perfcnt, dump) that a single-client
   driver can skip. Needed:
   - Power-up: `pd_g3d` on (Samsung pmucal sequence for `0xB1380008`), clocks, ACPM `FREQ_REQ
     0x0B040004`, then GPU_CONTROL L2/shader/tiler power-on and polling of the `*_READY` registers. Apply
     the G71 quirks from `panfrost_gpu.c` and `panfrost_issues.h`.
   - MMU: one address space (AS0) with 4 KiB AArch64 tables built from XNU physical pages. Map buffer
     objects and IOSurface pages. Handle faults through the MMU IRQ (93).
   - Job submission on JS slots (fragment and vertex/tiler/compute), fence by IRQ (92) or polling, and
     hang recovery by soft reset.
   - An IOUserClient speaking the **panfrost** UAPI subset above, plus syncobj emulation and an IOSurface
     import request, in the style of HackiOS's `RedwoodGPU.cpp`. Cache maintenance matters if the GPU is
     not IO-coherent on this SoC (unknown).
   - Thermal: read the GPU TMU and follow Samsung's step-down table (project safety rule 5).
2. **Mesa 25.0.7 for iOS.** Gallium `softpipe,panfrost`, OSMesa on, no LLVM, cross-built with the iOS SDK
   (Mesa needs the 16.x SDK headers; HackiOS uses theos plus `iPhoneOS16.5.sdk`). Add a Darwin `pan_kmod`
   backend or a `redwood-drm`-style libdrm shim that maps panfrost ioctls onto the user client. Backport any
   G71 fixes from `main` if 25.0.7 misbehaves on G71.
3. **Userland.** Port `eagl.c` and the backboardd hooks to 20H392. That means finding the `EAGLServer`
   and `GLESContext` vtables, CoreAnimation's GLES shader table and Metal's device array in our cache
   (local symbols are available, e.g. `CA::OGL::SW::*` already resolves). Also make dyld load an injected
   library in backboardd. HackiOS's userland is MIT, so reuse with attribution is allowed.
4. **Present path.** The Mali renders straight into the swap IOSurface. Next, the GPU (or DPP) does the
   scale into `0xcc000000`, which removes today's ~14 ms CPU scale. Zero-copy IDMA scan-out is the final
   step.

Expected payoff [estimate]. G71 MP20 at 546 MHz is roughly 11 Gpixel/s, from Mesa's rate of 1 pixel per
clock per core times 20 cores. A 1125x2436 frame is 2.7 Mpixel, so even 10x overdraw at 60 Hz is about
1.6 Gpixel/s. The GPU will not be the bottleneck. CPU time moves from rasterization to CoreAnimation's
layer work plus Mesa's driver overhead. A stable 60 Hz is plausible, but it depends on that overhead on
these cores.

### B in detail

- **DPP hardware scaling** (already W3.2 in the plan) or the **FIMG2D** blitter for the presenter's scale
  and copy. These free roughly a quarter of today's CPU budget per frame (snapshot ~5 ms plus scale
  ~14 ms on a saturated CPU) [estimate]. They do not touch CoreAnimation's own render time.
- **IOMFB multi-layer swaps**: only video and full-screen content would bypass the composite. Low value
  for UI smoothness [estimate].
- Reduce Transparency and Reduce Motion are still the cheapest CPU savings while CoreAnimation renders in
  software.

## 4. Recommendation and milestones

Recommendation: adopt **A** as the GPU direction and run **B** (DPP scaling) alongside it. Keep **C** as
a stretch goal. Drop **D** and **E**. Update `docs/plan.md` so that the "out of scope" line covers only
Apple's Metal/AGX stack and not GPU acceleration itself.

| Milestone | Content | Exit test | Effort [estimate] |
| --- | --- | --- | --- |
| G0 (desk, no phone) | Cross-build Mesa 25.0.7 (`softpipe,panfrost`, OSMesa, no LLVM) for arm64-ios on the Mac. Find the 20H392 addresses of `EAGLServer::renderer`, the `GLESContext` vtable, the GLES shader table and Metal's `_deviceArray`. Check whether our AMFI stand-in lets dyld honour `DYLD_INSERT_LIBRARIES` for backboardd. | Library links; address list written down with `ipsw` evidence | 1–2 weeks |
| **G1: the Mali does one job** | Diagnostic loader build (resets into TWRP, logs to pmsg, like `MNGS_DIAG`), or an XNU boot-arg probe. Read PMU G3D status; switch `pd_g3d` on; ACPM `FREQ_REQ 0x0B040004` at 260 MHz; read `GPU_ID` (expect `0x6000xxxx`, which gives the revision and decides errata); power L2 and cores; AS0 page table; one *Write value* job (Mesa `genxml/v6.xml`: Job Type 2, `Write Value Job Payload`). | The word appears in RAM, `JOB_IRQ_RAWSTAT` shows done, `GPU_FAULTSTATUS` is clean | 1–3 weeks |
| G2 | XNU Mali service with the panfrost UAPI subset and IOSurface import; Mesa panfrost talks to it; a test program draws a triangle through OSMesa into an IOSurface shown by SweetFramebuffer | Photo of a GPU-drawn triangle | 4–8 weeks |
| G3 | Softpipe-backed bridge first, as a functional check that CoreAnimation takes `GLESContext` on 20H392 (slow, correctness only). Then switch to panfrost. | Home screen drawn by the Mali; frame log shows render time per frame | 4–8 weeks |
| G4 | Missing CoreAnimation GLES functions, GPU or DPP scale to the panel, IRQ-driven fences, thermal, soak | swipe at 60 fps, 1 h without a GPU hang | open-ended |
| G5 (optional) | Metal plugin bundle plus AIR→NIR→Bifrost | Selected Metal apps run | months |

The G1 test only writes PMU G3D configuration, Mali registers and an ACPM frequency request from
Samsung's own table, with voltage picked by the APM firmware. It still needs a BOOT flash, so it must be
confirmed with the user like every other flash (AGENTS.md).

### Biggest unknowns and risks

1. **G71 under Panfrost in practice.** It is in the tables, but it is the least-tested Bifrost and
   non-conformant, and the S8's silicon revision is unknown until G1 reads `GPU_ID`. Errata may cause
   hangs or misrendering. Mitigation: start with Mesa 25.0.7 and backport fixes from `main`; the Linux
   panfrost quirks are the reference.
2. **The OSMesa dead end.** Mesa 25.1 and later have no OSMesa. Long term this means staying on 25.0.x or
   writing our own small gallium frontend (or an EGL surfaceless port).
3. **G3D power-up sequence.** `pd_g3d` goes through Samsung's pmucal tables and may need CMU/SYSREG steps.
   The ACPM G3D domain has not been tried. If the domain is already on after S-Boot, this is much easier.
4. **CoreAnimation's GLES backend is a legacy path** with fewer than half of the CA functions. Visual gaps
   (blurs, vibrancy, materials) need hand-written stand-ins, and some may stay wrong. HackiOS's notes help
   here.
5. **Version drift.** Every hook is a raw address in the shared cache. 16.5 and 16.7.16 differ, so all of
   them must be found again and checked at run time, as HackiOS does.
6. **dyld injection policy** under our AMFI stand-in is untested. We control the kernel, so this can be
   fixed (HackiOS uses a kernel-side injection), but it is work.
7. **Memory.** XNU has 512 MiB today (the 2 GiB attempt c13 panicked). Mesa plus GPU buffers plus
   1125x2436 surfaces (~11 MB each) need headroom, so the 2 GiB work (W2) should land first or alongside.
8. **CPU-side overhead.** Even with the GPU, CoreAnimation and Mesa command building run on the CPU, and
   the gain could be smaller than hoped. G3 measures this before G4 polish starts.
9. **HackiOS's claims are unverified** and very new (published 2026-10-22). Their design is
   well documented in the code, but their Adreno numbers do not transfer to Mali.

## Sources

Local evidence (not in git): `research/dyld/20H392__iPhone10,3_6` (dyld cache, via `ipsw dyld str`,
`ipsw dyld info --dylibs`, `ipsw dyld objc class --image AGXMetalA11`); the 20H392 kernelcache extracted
from `research/ipsw/iPhone10,3_16.7.16_20H392_Restore.ipsw` (`ipsw kernel kexts`, prelink plist strings);
`logs/ect/dvfs_table.txt`, `asv_summary.txt`; `DREAMLTE_PROGRESS.md`, `docs/plan.md`, `docs/spec.md`.

- HackiOS: <https://github.com/oosexgang/hackios-xnu-kernel> (README, `xnu/files/bsd/dev/redwood_adreno.c`,
  `xnu/files/iokit/Kernel/RedwoodGPU.cpp`), <https://github.com/oosexgang/hackios-xnu-userland> (README,
  `userland/src/eagl.c`, `inject.c`, `gl_osmesa.c`, `metal_plugin.plist`, `tools/mk-mesa.sh`,
  `tools/mesa/redwood-drm/redwood_drm.c`, `tools/mesa/freedreno-darwin.patch`, `docs/air-report.md`)
- Mesa Panfrost docs: <https://docs.mesa3d.org/drivers/panfrost.html>; source at
  <https://gitlab.freedesktop.org/mesa/mesa> (`src/panfrost/model/pan_model.c` on main,
  `src/panfrost/lib/pan_props.c` and `docs/drivers/panfrost.rst` at tag `mesa-25.0.7`,
  `src/panfrost/lib/kmod/panfrost_kmod.c`, `src/gallium/drivers/panfrost/*`,
  `src/panfrost/vulkan/meson.build`, `src/panfrost/genxml/v6.xml`; commit `5eab85a5`; OSMesa frontend
  present at `mesa-25.0.7`, absent at `mesa-25.1.0`)
- Linux panfrost: <https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/panfrost>
  (`panfrost_gpu.c`, `panfrost_issues.h`, `panfrost_mmu.c`, `panfrost_drv.c`)
- Samsung Exynos 8895 kernel: <https://github.com/exynos8895/android_kernel_samsung_universal8895>
  (branch lineage-19.1: `arch/arm64/boot/dts/exynos/exynos8895.dtsi`, `exynos8895-pm-domains.dtsi`,
  `include/dt-bindings/clock/exynos8895.h`, `drivers/gpu/arm/b_r16p0/platform/exynos/`)
- Asahi Lina, "Tales of the M1 GPU": <https://asahilinux.org/2022/11/tales-of-the-m1-gpu/>
- Alyssa Rosenzweig, "Dissecting the Apple M1 GPU, part II": <https://alyssarosenzweig.ca/blog/asahi-gpu-part-2.html>
- eShard, "Emulating iOS 14 with QEMU": <https://eshard.com/blog/emulating-ios-14-with-qemu>
- qemu-t8030: <https://github.com/TrungNguyen1909/qemu-t8030> (archived); <https://github.com/ChefKissInc/QEMUAppleSilicon>
- Hackaday on iOS in QEMU and software rendering: <https://hackaday.com/tag/qemu>
- Corellium getting started: <https://support.corellium.com/getting-started/>
- Darling Indium: <https://github.com/darlinghq/indium>
