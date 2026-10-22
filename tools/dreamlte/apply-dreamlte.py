#!/usr/bin/env python3
"""Apply the Galaxy S8 (dreamlte, Exynos 8895) hardware layer to an
A5XELTE-patched xnu tree (tools-s8/prep-xnu-tree.sh output).

The A5XELTE machine config and its guards are reused unchanged. What
differs on the S8 is expressed as A5_* macros in A5XELTE.h, with the A5
values kept for a non-DREAMLTE build. Every replacement asserts its
expected match count. Usage: apply-dreamlte.py XNU_TREE
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def edit(rel, pairs):
    path = tree / rel
    text = path.read_text()
    for old, new, count in pairs:
        found = text.count(old)
        if found != count:
            raise SystemExit(f'{rel}: expected {count}x {old!r}, found {found}')
        text = text.replace(old, new)
    path.write_text(text)
    print(f'edited {rel}')


edit('pexpert/pexpert/arm64/A5XELTE.h', [(
    '#define A5XELTE_VTIMER_INTID         27\n',
    '#define A5XELTE_VTIMER_INTID         27\n'
    '\n'
    '/*\n'
    ' * Samsung Galaxy S8 (SM-G950F, dreamlte, Exynos 8895) build of this port.\n'
    ' * Same Samsung S-Boot handoff, GIC-400, 26 MHz timer and Cortex-A53 boot\n'
    ' * cluster as the A5; the board facts that differ are below.\n'
    ' */\n'
    '#define DREAMLTE                      1\n'
    '#if defined(DREAMLTE)\n'
    '#define A5_FB_PHYS                    0xcc000000  /* s3cfb.bootloaderfb */\n'
    '#define A5_FB_WIDTH                   1440\n'
    '#define A5_FB_HEIGHT                  2960\n'
    '#define A5_FB_STRIDE                  5760        /* bytes per row */\n'
    '#define A5_FB_2M_BLOCKS               9           /* 1440*2960*4 bytes */\n'
    '#define A5_ENTRY_MARKER_PHYS          0x920ff000  /* slot in the loader\'s pstore text */\n'
    '#define A5_BOOT_MPIDR                 0x100       /* first Cortex-A53 */\n'
    '#define A5_KEYS_DEFAULT               1           /* S8 keys: GPA0/GPA2 */\n'
    '#define A5_TOUCH_DEFAULT              0           /* A5 ZT7538 path absent */\n'
    '#define A5_KLOG_PHYS                  0x92180000  /* TWRP ramoops pmsg zone */\n'
    '#define A5_KLOG_SIZE                  0x80000\n'
    '#define A5_KLOG2_PHYS                 0x92004000  /* TWRP 3.4.0 (G950N) pmsg zone */\n'
    '#define A5_KLOG2_SIZE                 0x4000\n'
    '/* Samsung\'s S8 kernel DT: arch timer "use-physical-timer". The EL1\n'
    ' * virtual timer is not usable on the secondary cores (boot 22/24: 4\n'
    ' * CPUs up, then sleeping threads and workqueue threads never ran).\n'
    ' * Use the non-secure physical timer, PPI 14 = INTID 30. */\n'
    '#define A5_USE_PHYS_TIMER             1\n'
    '#undef A5XELTE_VTIMER_INTID\n'
    '#define A5XELTE_VTIMER_INTID         30\n'
    '#else\n'
    '#define A5_FB_PHYS                    0x67000000\n'
    '#define A5_FB_WIDTH                   1080\n'
    '#define A5_FB_HEIGHT                  1920\n'
    '#define A5_FB_STRIDE                  4320\n'
    '#define A5_FB_2M_BLOCKS               5\n'
    '#define A5_ENTRY_MARKER_PHYS          0x469ff100\n'
    '#define A5_BOOT_MPIDR                 0\n'
    '#define A5_KEYS_DEFAULT               1\n'
    '#define A5_TOUCH_DEFAULT              1\n'
    '#endif\n', 1)])

# Early assembly: entry/MMU/stage blocks are 36x21 px at x=240, rows 88..216.
stage_rows = {'0x6705d0c0': 88, '0x6707ecc0': 120, '0x670a08c0': 152,
              '0x670c24c0': 184, '0x670e40c0': 216}
pairs = [('MOV64\tx10, 0x469ff100', 'MOV64\tx10, A5_ENTRY_MARKER_PHYS', 1)]
for addr, row in stage_rows.items():
    pairs.append((f'MOV64\tx10, {addr}',
                  f'MOV64\tx10, (A5_FB_PHYS + {row} * A5_FB_STRIDE + 240 * 4)', 1))
pairs += [
    ('mov\t\tx15, #4320', 'mov\t\tx15, #A5_FB_STRIDE', 2),
    ('mov\tx15, #4320', 'mov\tx15, #A5_FB_STRIDE', 3),
    ('MOV64\tx0, 0x67000000\n\tmov\t\tx4, x0\n\tmov\t\tx5, #5\n',
     'MOV64\tx0, A5_FB_PHYS\n\tmov\t\tx4, x0\n\tmov\t\tx5, #A5_FB_2M_BLOCKS\n', 1),
    ('MOV64\tx10, 0x67000000', 'MOV64\tx10, A5_FB_PHYS', 1),
    ('mov\t\tx13, #5\nLa5_fb_attr:', 'mov\t\tx13, #A5_FB_2M_BLOCKS\nLa5_fb_attr:', 1),
    ('MOV64\tx4, 0x67000000\n\tmov\tx5, #4320',
     'MOV64\tx4, A5_FB_PHYS\n\tmov\tx5, #A5_FB_STRIDE', 1),
    ('mov\tx13, #8640', 'mov\tx13, #(2 * A5_FB_STRIDE)', 1),
    ('/* Five 2 MiB blocks map the 1080x1920 scanout buffer VA=PA.',
     '/* A5_FB_2M_BLOCKS 2 MiB blocks map the scanout buffer VA=PA.', 1),
]
edit('osfmk/arm64/start.s', pairs)

edit('osfmk/arm64/sweet_early.c', [
    ('#define A5_FB ((volatile uint32_t *)(uintptr_t)0x67000000)\n#define A5_STRIDE 1080u\n',
     '#define A5_FB ((volatile uint32_t *)(uintptr_t)A5_FB_PHYS)\n#define A5_STRIDE (A5_FB_STRIDE / 4u)\n', 1),
    ('#define A5_COLS (1080u / A5_COL_WIDTH)', '#define A5_COLS (A5_FB_WIDTH / A5_COL_WIDTH)', 1),
    ('(1920u / A5_ROW_HEIGHT)', '(A5_FB_HEIGHT / A5_ROW_HEIGHT)', 1),
    ('for (unsigned int x = 0; x < A5_STRIDE; x++) {', 'for (unsigned int x = 0; x < A5_FB_WIDTH; x++) {', 1),
])

edit('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ('#include <pexpert/pexpert.h>\n',
     '#include <pexpert/pexpert.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('\tfPhys = 0x67000000ULL;\n\tfWidth = 1080;\n\tfHeight = 1920;\n\tfRowBytes = 4320;\n',
     '\tfPhys = A5_FB_PHYS;\n\tfWidth = A5_FB_WIDTH;\n\tfHeight = A5_FB_HEIGHT;\n\tfRowBytes = A5_FB_STRIDE;\n', 1),
])

edit('iokit/Kernel/arm/SweetCPU.cpp', [
    ('\tuint32_t expected_mpidr = cpu->cpu_id < 4 ? cpu->cpu_id :\n\t    0x100U + cpu->cpu_id - 4;\n',
     '#if defined(DREAMLTE)\n'
     '\t/* Exynos 8895: logical 0..3 = A53 MPIDR 0x100..0x103 (boot cluster);\n'
     '\t * logical 4..7 = Mongoose M2 MPIDR 0..3. */\n'
     '\tuint32_t expected_mpidr = cpu->cpu_id < 4 ? 0x100U + cpu->cpu_id :\n'
     '\t    cpu->cpu_id - 4;\n'
     '#else\n'
     '\tuint32_t expected_mpidr = cpu->cpu_id < 4 ? cpu->cpu_id :\n'
     '\t    0x100U + cpu->cpu_id - 4;\n'
     '#endif\n', 1),
    ('\t/* The A5 ADT describes CPU0 as the boot CPU. */\n'
     '\tif (topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0) {\n'
     '\t\tpanic(CPU_PORT_NAME " CPU: expected boot MPIDR 0 and logical CPU 0");\n',
     '\t/* The ADT describes logical CPU 0 as the boot CPU. */\n'
     '\tif (topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != A5_BOOT_MPIDR) {\n'
     '\t\tpanic(CPU_PORT_NAME " CPU: expected boot MPIDR %#x and logical CPU 0", A5_BOOT_MPIDR);\n', 1),
])

edit('pexpert/arm/pe_serial.c', [
    ('static uint32_t fbcon_cols, fbcon_rows, fbcon_col, fbcon_row;\n',
     'static uint32_t fbcon_cols, fbcon_rows, fbcon_col, fbcon_row;\n'
     '\n'
     '#if defined(DREAMLTE)\n'
     '/*\n'
     ' * Copy of the console in TWRP\'s ramoops pmsg zone, in persistent_ram\n'
     ' * format (sig "DBGC", start, size, ring data), appended to the loader\'s\n'
     ' * text there; TWRP shows it as /sys/fs/pstore/pmsg-ramoops-0 after a warm\n'
     ' * reset. Two layouts: the upstream author\'s TWRP (2 MiB ramoops, pmsg at\n'
     ' * 0x92180000) and TWRP 3.4.0 on the galaxios8 G950N (32 KiB ramoops,\n'
     ' * 16 KiB pmsg at 0x92004000); both get the text. The small zone wraps,\n'
     ' * so it keeps the last ~16 KiB, which is where a panic ends up.\n'
     ' */\n'
     'struct klog_zone {\n'
     '\tuint64_t phys, len;\n'
     '\tvolatile uint32_t *hdr;\n'
     '\tvolatile uint8_t *data;\n'
     '\tuint32_t cap, start, size;\n'
     '};\n'
     'static struct klog_zone klog_zones[] = {\n'
     '\t{ .phys = A5_KLOG_PHYS, .len = A5_KLOG_SIZE },\n'
     '\t{ .phys = A5_KLOG2_PHYS, .len = A5_KLOG2_SIZE },\n'
     '};\n'
     'static void klog_putc(uint8_t c);\n'
     '\n'
     'static void\n'
     'klog_init(void)\n'
     '{\n'
     '\tfor (unsigned i = 0; i < sizeof(klog_zones) / sizeof(klog_zones[0]); i++) {\n'
     '\t\tstruct klog_zone *z = &klog_zones[i];\n'
     '\t\tz->hdr = (volatile uint32_t *)ml_io_map(z->phys, z->len);\n'
     '\t\tif (z->hdr == NULL) {\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '\t\tz->cap = (uint32_t)z->len - 12;\n'
     '\t\t/* Continue after the loader\'s text if its header is sane. */\n'
     '\t\tif (z->hdr[0] == 0x43474244u && z->hdr[1] == z->hdr[2] && z->hdr[2] < z->cap) {\n'
     '\t\t\tz->start = z->size = z->hdr[2];\n'
     '\t\t} else {\n'
     '\t\t\tz->hdr[0] = 0x43474244u;\n'
     '\t\t\tz->start = z->size = 0;\n'
     '\t\t}\n'
     '\t\tz->data = (volatile uint8_t *)(z->hdr + 3);\n'
     '\t}\n'
     '\tconst char *banner = "\\n--- XNU console ---\\n";\n'
     '\tfor (; *banner; banner++) {\n'
     '\t\tklog_putc((uint8_t)*banner);\n'
     '\t}\n'
     '\ts8_restart_init();\n'
     '}\n'
     '\n'
     'static void\n'
     'klog_putc(uint8_t c)\n'
     '{\n'
     '\tfor (unsigned i = 0; i < sizeof(klog_zones) / sizeof(klog_zones[0]); i++) {\n'
     '\t\tstruct klog_zone *z = &klog_zones[i];\n'
     '\t\tif (z->data == NULL) {\n'
     '\t\t\tcontinue;\n'
     '\t\t}\n'
     '\t\tz->data[z->start] = c;\n'
     '\t\tz->start = (z->start + 1) % z->cap;\n'
     '\t\tif (z->size < z->cap) {\n'
     '\t\t\tz->size++;\n'
     '\t\t}\n'
     '\t\tz->hdr[1] = z->start;\n'
     '\t\tz->hdr[2] = z->size;\n'
     '\t}\n'
     '}\n'
     '#endif\n', 1),
    ('fbcon_transmit_data(uint8_t c)\n{\n',
     'fbcon_transmit_data(uint8_t c)\n{\n'
     '#if defined(DREAMLTE)\n'
     '\tklog_putc(c);\n'
     '#endif\n', 1),
    ('\tfbcon_base = (volatile uint32_t *)ml_io_map_wcomb(reg[0], reg[1]);\n',
     '\tfbcon_base = (volatile uint32_t *)ml_io_map_wcomb(reg[0], reg[1]);\n'
     '#if defined(DREAMLTE)\n'
     '\tklog_init();\n'
     '#endif\n', 1),
])

# Restart, panic and power-off. Nothing set PE_halt_restart, so a panic or an
# iOS restart left the CPU spinning. Samsung's sec_reboot.c / exynos-reboot.c:
# INFORM2 = 0x12345678 (POWER_RESET), INFORM3 = reset reason, PMU SWRESET;
# power off = PS_HOLD (PMU 0x330c bit 8) low. A panic resets into TWRP when
# the test boot-arg s8autoreset is set (the pmsg zone then holds the panic),
# otherwise it restarts into iOS.
edit('pexpert/arm/pe_serial.c', [
    ('#if defined(DREAMLTE)\n/*\n * Copy of the console in TWRP',
     '#if defined(DREAMLTE)\n'
     'extern int (*PE_halt_restart)(unsigned int type);\n'
     'static volatile uint32_t *s8_pmu;\n'
     'static uint32_t s8_panic_to_recovery;\n'
     '\n'
     'static int\n'
     's8_halt_restart(unsigned int type)\n'
     '{\n'
     '\t/* IOPlatformExpert.h: 0 halt, 1 restart, 4 panic restart,\n'
     '\t * 9 panic restart without callouts; other types are notifications. */\n'
     '\tif (s8_pmu == NULL || (type != 0 && type != 1 && type != 4 && type != 9)) {\n'
     '\t\treturn 0;\n'
     '\t}\n'
     '\tif (type == 0) {\n'
     '\t\ts8_pmu[0x330c / 4] &= ~0x100u;   /* PS_HOLD low: power off */\n'
     '\t\tfor (;;) {\n'
     '\t\t}\n'
     '\t}\n'
     '\tuint32_t reason = (type != 1 && s8_panic_to_recovery) ?\n'
     '\t    0x12345674u : 0x12345670u;     /* REASON_RECOVERY / UNKNOWN */\n'
     '\ts8_pmu[0x808 / 4] = 0x12345678u;     /* INFORM2: SEC_POWER_RESET */\n'
     '\ts8_pmu[0x80c / 4] = reason;          /* INFORM3 */\n'
     '\t__asm__ volatile ("dsb sy" ::: "memory");\n'
     '\ts8_pmu[0x400 / 4] = 1;               /* SWRESET */\n'
     '\tfor (;;) {\n'
     '\t}\n'
     '}\n'
     '\n'
     'static void\n'
     's8_restart_init(void)\n'
     '{\n'
     '\tuint32_t secs = 0;\n'
     '\n'
     '\ts8_pmu = (volatile uint32_t *)ml_io_map(0x16480000, 0x4000);\n'
     '\ts8_panic_to_recovery = PE_parse_boot_argn("s8autoreset", &secs, sizeof(secs)) && secs != 0;\n'
     '\tPE_halt_restart = s8_halt_restart;\n'
     '}\n'
     '\n'
     '/*\n * Copy of the console in TWRP', 1),
])

# Boots 3, 5, 6 and 7 hung right after the kernel version line, before
# pmap_startup's "init/release time"; boots 1, 2, 3b and 5b got past it with
# the same image. The physical aperture slide (1 GiB range, early_random) is
# the boot-to-boot variable there (gPicBase differs on every photo). Pin it
# on the S8 and mark vm_page_bootstrap's steps in the console.
edit('osfmk/arm64/arm_vm_init.c', [
    ('\tuint64_t physmap_slide = early_random() & ARM64_PHYSMAP_SLIDE_MASK & ~((uint64_t)PAGE_MASK);\n',
     '#if defined(DREAMLTE)\n'
     '\tuint64_t physmap_slide = 0;\n'
     '#else\n'
     '\tuint64_t physmap_slide = early_random() & ARM64_PHYSMAP_SLIDE_MASK & ~((uint64_t)PAGE_MASK);\n'
     '#endif\n', 1),
    # Boot 14: a 4 MiB larger kernel (sweet_ufs log ring) moved
    # topOfKernelData and hung pmap_startup again. Only XNU_MONITOR builds
    # round physmap_base to a twig (2 MiB with 4 KiB pages); do it here too.
    # The real cause of the vm_page_bootstrap hangs (boots 3-7, 14, 15):
    # arm_vm_init pre-builds L3 tables only for (2 + 10 * 256 MiB-segments)
    # MiB + the boot_args framebuffer past dynamic_memory_begin. Our loader
    # passes no Video, yet the console maps the 17 MiB S8 framebuffer there
    # first; when pmap_steal_memory then needs a table beyond the budget,
    # pmap_enter cannot allocate one yet. With 4 KiB pages one L3 table spans
    # 2 MiB (32 MiB with 16 KiB), so add room for the framebuffer and 64 MiB.
    ('\tva_l1_end += round_page(args->Video.v_height * args->Video.v_rowBytes);\n',
     '\tva_l1_end += round_page(args->Video.v_height * args->Video.v_rowBytes);\n'
     '#if defined(DREAMLTE)\n'
     '\tva_l1_end += round_page((vm_size_t)A5_FB_STRIDE * A5_FB_HEIGHT) + (64ULL << 20);\n'
     '#endif\n', 1),
    ('\tphysmap_base += physmap_slide;\n',
     '\tphysmap_base += physmap_slide;\n'
     '#if defined(DREAMLTE) && !XNU_MONITOR\n'
     '\tphysmap_base = ROUND_TWIG(physmap_base);\n'
     '#endif\n', 1),
])

edit('osfmk/vm/vm_resident.c', [
    ('\tpmap_startup(&virtual_space_start, &virtual_space_end);\n',
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tkprintf("A5 vm: vm_page_bootstrap -> pmap_startup\\n");\n'
     '#endif\n'
     '\tpmap_startup(&virtual_space_start, &virtual_space_end);\n', 1),
    ('\tvm_pages = (vm_page_t) pmap_steal_freeable_memory(npages * sizeof *vm_pages);\n',
     '\tvm_pages = (vm_page_t) pmap_steal_freeable_memory(npages * sizeof *vm_pages);\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tkprintf("A5 vm: npages=%u vm_pages=%p mem_sz=%#llx\\n", npages, vm_pages, (unsigned long long)mem_sz);\n'
     '#endif\n', 1),
    ('\t\tif (!pmap_next_page(&phys_page)) {\n\t\t\tbreak;\n\t\t}\n\n\t\tif (phys_page < max_valid_low_ppnum) {\n',
     '\t\tif (!pmap_next_page(&phys_page)) {\n\t\t\tbreak;\n\t\t}\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\t\tif ((i & 0x7fff) == 0) {\n'
     '\t\t\tkprintf("A5 vm: page %u ppn %#x\\n", i, phys_page);\n'
     '\t\t}\n'
     '#endif\n'
     '\n\t\tif (phys_page < max_valid_low_ppnum) {\n', 1),
    ('\tvm_page_pages = vm_pages_count; /* used to report to user space */\n',
     '\tvm_page_pages = vm_pages_count; /* used to report to user space */\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tkprintf("A5 vm: %u pages initialized, releasing\\n", vm_pages_count);\n'
     '#endif\n', 1),
])

# ---- Galaxy S8 internal UFS: the SWEET driver with the Exynos 8895 quirks ----
# Proven by the loader probe (shim/dreamlte/ufs_probe.c, S8 boot 13): HCE
# reads 0 while running, UTRD sizes in bytes, 16-byte PRDT entries of at
# most 4 KiB, NEXUS_TYPE bit per SCSI slot, and the device stays NOT READY
# (04/02) until START STOP UNIT (power condition ACTIVE) to W-LUN 0xD0.
edit('bsd/dev/sweet_ufs.c', [
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\n#include <sys/param.h>\n',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '#include <sys/param.h>\n', 1),
    ('#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */\n',
     '#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */\n'
     '\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '/*\n'
     ' * Galaxy S8 (Exynos 8895) host, per the Samsung 4.4 driver and the S8\n'
     ' * loader probe (shim/dreamlte/ufs_probe.c): UTRD offsets and lengths are\n'
     ' * in bytes (UFSHCD_QUIRK_BROKEN_DWORD_UTRD), PRDT entries are 16 bytes and\n'
     ' * cover at most 4 KiB (HCI_TXPRDT_ENTRY_SIZE 12), each SCSI slot needs its\n'
     ' * HCI_UTRL_NEXUS_TYPE bit, the standard HCE register reads 0 while the host\n'
     ' * runs (UFSHCD_QUIRK_USE_OF_HCE) and UTRLCLR is inverted\n'
     ' * (UFSHCD_QUIRK_BROKEN_REQ_LIST_CLR). S-Boot leaves the device stopped\n'
     ' * (sense 02/04/02): START STOP UNIT, power condition ACTIVE, to the UFS\n'
     ' * Device well-known LUN wakes it.\n'
     ' */\n'
     '#define EXYNOS_UFS              1\n'
     '#define VS_UTRL_NEXUS_TYPE      0x40\n'
     '#define UFS_WLUN_DEVICE         0xd0\n'
     '#define UFS_ADT_NODE            "/dreamlte-ufs"\n'
     '#define UFS_DEFAULT_PART        "USERDATA"\n'
     '#define PRDT_BYTES(field)       (field)\n'
     '#else\n'
     '#define UFS_ADT_NODE            "/sweet-ufs"\n'
     '#define UFS_DEFAULT_PART        "userdata"\n'
     '#define PRDT_BYTES(field)       ((field) * 16u)\n'
     '#endif\n', 1),
    ('#define PRDT_CHUNK      (256u << 10)            /* PRDT byte count field: 18 bits */\n',
     '#if defined(EXYNOS_UFS)\n'
     '#define PRDT_CHUNK      4096u                   /* HCI_TXPRDT_ENTRY_SIZE 12 */\n'
     '#else\n'
     '#define PRDT_CHUNK      (256u << 10)            /* PRDT byte count field: 18 bits */\n'
     '#endif\n', 1),
    ('\tvm_offset_t regs;\n',
     '\tvm_offset_t regs;\n\tvm_offset_t vs_regs;                    /* Exynos vendor HCI registers */\n', 1),
    ('\tutrd->resp = (512u / 4) << 16 | (512u / 4);\n',
     '#if defined(EXYNOS_UFS)\n'
     '\tutrd->resp = 512u << 16 | 512u;\n'
     '#else\n'
     '\tutrd->resp = (512u / 4) << 16 | (512u / 4);\n'
     '#endif\n', 1),
    ('\t\tutrd->prdt = (1024u / 4) << 16 | n;\n',
     '#if defined(EXYNOS_UFS)\n'
     '\t\tutrd->prdt = 1024u << 16 | (n * (uint32_t)sizeof(ucd->prdt[0]));\n'
     '#else\n'
     '\t\tutrd->prdt = (1024u / 4) << 16 | n;\n'
     '#endif\n', 1),
    ('\t\t\t    (utrd->prdt & 0xffff) * sizeof(ucd->prdt[0]));\n',
     '\t\t\t    PRDT_BYTES(utrd->prdt & 0xffff));\n', 1),
    ('\tufs_wr(REG_UTRLDBR, 1);\n',
     '#if defined(EXYNOS_UFS)\n'
     '\t*(volatile uint32_t *)(su.vs_regs + VS_UTRL_NEXUS_TYPE) |= 1;  /* slot 0: SCSI */\n'
     '#endif\n'
     '\tufs_wr(REG_UTRLDBR, 1);\n', 1),
    ('\t\t\tufs_wr(REG_UTRLCLR, ~1u);\n',
     '#if !defined(EXYNOS_UFS)\n'
     '\t\t\tufs_wr(REG_UTRLCLR, ~1u);\n'
     '#endif\n', 1),
    ('\tif (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {\n',
     '#if defined(EXYNOS_UFS)\n'
     '\t(void)hce;\n'
     '\tif (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {\n'
     '#else\n'
     '\tif (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {\n'
     '#endif\n', 1),
    ('\tchar want[64] = "userdata";\n', '\tchar want[64] = UFS_DEFAULT_PART;\n', 1),
    ('\tif (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||\n'
     '\t    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||\n'
     '\t    size < 2 * sizeof(uintptr_t)) {\n'
     '\t\tprintf("SWEET UFS: no /sweet-ufs node in the device tree\\n");\n',
     '\tif (SecureDTLookupEntry(NULL, UFS_ADT_NODE, &node) != kSuccess ||\n'
     '\t    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||\n'
     '#if defined(EXYNOS_UFS)\n'
     '\t    size < 4 * sizeof(uintptr_t)) {\n'
     '#else\n'
     '\t    size < 2 * sizeof(uintptr_t)) {\n'
     '#endif\n'
     '\t\tprintf("SWEET UFS: no " UFS_ADT_NODE " node in the device tree\\n");\n', 1),
    ('\tsu.regs = ml_io_map(reg[0], reg[1]);\n',
     '\tsu.regs = ml_io_map(reg[0], reg[1]);\n'
     '#if defined(EXYNOS_UFS)\n'
     '\tsu.vs_regs = ml_io_map(reg[2], reg[3]);\n'
     '#endif\n', 1),
    ('\terror = ufs_take_over_host();\n',
     '\terror = ufs_take_over_host();\n'
     '#if defined(EXYNOS_UFS)\n'
     '\tif (!error) {\n'
     '\t\tstatic const uint8_t ssu_active[6] = { 0x1b, 0, 0, 0, 0x10, 0 };\n'
     '\t\tsu.lun = UFS_WLUN_DEVICE;\n'
     '\t\terror = ufs_scsi(ssu_active, sizeof(ssu_active), 0, false);\n'
     '\t\tsu.lun = 0;\n'
     '\t\tprintf("SWEET UFS: START STOP UNIT (ACTIVE) to the device W-LUN: %d\\n", error);\n'
     '\t}\n'
     '#endif\n', 1),
    ('\tsweet_sample_start();\n\t/* Input drivers (HID services in iokit/Kernel/arm/hid/SweetHID.cpp). */\n'
     '\tsweet_keys_init();\n\tsweet_touch_init();\n\tsweet_usb_probe();\n',
     '\tsweet_sample_start();\n'
     '#if defined(ARM64_BOARD_CONFIG_SWEET)\n'
     '\t/* Input drivers (HID services in iokit/Kernel/arm/hid/SweetHID.cpp). */\n'
     '\tsweet_keys_init();\n\tsweet_touch_init();\n\tsweet_usb_probe();\n'
     '#endif\n', 1),
])

# The SWEET user-stack sampler (bsd/dev/sweet_sample.c; its kernel hook in
# exception.c is already built for A5XELTE): stacks of SpringBoard and
# backboardd every 2 minutes, to see where 4-CPU rendering stalls (boot 22).
edit('bsd/dev/sweet_sample.c', [
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\n/*\n * SWEET process sampler',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n/*\n * SWEET process sampler', 1),
])

# rd=ufsroot0 also on the A5XELTE machine config (the S8 build).
edit('iokit/bsddev/IOKitBSDInit.cpp', [
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\n//\n//\tSWEET: root on a GPT partition of the internal UFS (bsd/dev/sweet_ufs.c)\n',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n//\n'
     '//\tSWEET / Galaxy S8: root on a GPT partition of the internal UFS (bsd/dev/sweet_ufs.c)\n', 1),
])

# Console copy into sweet_ufs.c's ring (written to the end of the root
# partition every ufslogsecs when ufsrw=1): longer than pstore, no bit flips.
edit('osfmk/console/serial_console.c', [
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\nextern void sweet_log_append',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\nextern void sweet_log_append', 1),
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\n\tsweet_log_append(c, size);',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n\tsweet_log_append(c, size);', 1),
])

# Physical instead of virtual EL1 timer on the S8 (A5_USE_PHYS_TIMER).
edit('osfmk/arm64/machine_routines.c', [
    ('\t\t__builtin_arm_wsr64("CNTV_TVAL_EL0", (uint64_t)dec_value);\n',
     '#if defined(A5_USE_PHYS_TIMER)\n'
     '\t\t__builtin_arm_wsr64("CNTP_TVAL_EL0", (uint64_t)dec_value);\n'
     '#else\n'
     '\t\t__builtin_arm_wsr64("CNTV_TVAL_EL0", (uint64_t)dec_value);\n'
     '#endif\n', 1),
    ('\t\twide_val = __builtin_arm_rsr64("CNTV_TVAL_EL0");\n',
     '#if defined(A5_USE_PHYS_TIMER)\n'
     '\t\twide_val = __builtin_arm_rsr64("CNTP_TVAL_EL0");\n'
     '#else\n'
     '\t\twide_val = __builtin_arm_rsr64("CNTV_TVAL_EL0");\n'
     '#endif\n', 1),
    ('\tuint64_t cntv_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");\n',
     '#if defined(A5_USE_PHYS_TIMER)\n'
     '\tuint64_t cntv_ctl = __builtin_arm_rsr64("CNTP_CTL_EL0");\n'
     '#else\n'
     '\tuint64_t cntv_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");\n'
     '#endif\n', 1),
    ('\t__builtin_arm_wsr64("CNTV_CTL_EL0", cntvctl);\n'
     '\t/* disable the physical timer as a precaution, as its registers reset to architecturally unknown values */\n'
     '\t__builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);\n',
     '#if defined(A5_USE_PHYS_TIMER)\n'
     '\t__builtin_arm_wsr64("CNTP_CTL_EL0", cntvctl);   /* same ENABLE bit */\n'
     '\t__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);\n'
     '#else\n'
     '\t__builtin_arm_wsr64("CNTV_CTL_EL0", cntvctl);\n'
     '\t/* disable the physical timer as a precaution, as its registers reset to architecturally unknown values */\n'
     '\t__builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);\n'
     '#endif\n', 1),
])

edit('pexpert/arm/pe_fiq.c', [
    ('\t    a5_gic_read32(a5_gicd_base, 0x080) | (1U << 27) | (1U << 1));\n',
     '\t    a5_gic_read32(a5_gicd_base, 0x080) | (1U << A5XELTE_VTIMER_INTID) | (1U << 1));\n', 1),
    ('\ta5_gic_write32(a5_gicd_base, 0x100, (1U << 27) | (1U << 1));\n',
     '\ta5_gic_write32(a5_gicd_base, 0x100, (1U << A5XELTE_VTIMER_INTID) | (1U << 1));\n', 1),
])

# IOKit platform expert / CPU personalities: also match the S8 names.
# Present the S8 panel as the iPhone X screen (1125x2436, centered on the
# 1440x2960 panel), like SWEET does: iOS has no layout data for 1440x2960.
edit('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ('#if !defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tif (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {\n',
     '#if defined(DREAMLTE)\n'
     '\tif (!native) {\n'
     '\t\tfLogicalW = SWEET_FB_LOGICAL_W;\n'
     '\t\tfLogicalH = SWEET_FB_LOGICAL_H;\n'
     '\t}\n'
     '#elif !defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tif (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {\n', 1),
])

edit('iokit/KernelConfigTables.cpp', [
    ("    \"     'IONameMatch'     = 'samsung,a5xelte-display';\"\n",
     "    \"     'IONameMatch'     = ('samsung,a5xelte-display', 'samsung,dreamlte-display');\"\n", 1),
    ("    \"     'IONameMatch'     = 'samsung,a5xelte';\"\n",
     "    \"     'IONameMatch'     = ('samsung,a5xelte', 'samsung,dreamlte');\"\n", 1),
])

# A5 touch probe maps Exynos7580 I2C/GPIO blocks: a bus error on the S8.
edit('bsd/dev/a5xelte_touch.c', [
    ('#include <pexpert/pexpert.h>\n',
     '#include <pexpert/pexpert.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('a5xelte_touch_probe(void)\n{\n\tint enabled = 1;\n',
     'a5xelte_touch_probe(void)\n{\n\tint enabled = A5_TOUCH_DEFAULT; /* A5 touch: off on the S8 */\n', 1),
])
# No platform clock on the S8 (iOS showed 31 Dec 1969). The loader reads the
# S2MPS17 RTC and passes s8rtc = UTC seconds at system-counter zero.
edit('iokit/Kernel/IOPlatformExpert.cpp', [
    ('\tif (gIOPlatform) {\n\t\tgIOPlatform->getUTCTimeOfDay(secs, &nsecs);\n\t}\n\n\tassert(nsecs < NSEC_PER_SEC);\n',
     '\tif (gIOPlatform) {\n\t\tgIOPlatform->getUTCTimeOfDay(secs, &nsecs);\n\t}\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tuint64_t s8rtc = 0;\n'
     '\tif (*secs == 0 && PE_parse_boot_argn("s8rtc", &s8rtc, sizeof(s8rtc)) && s8rtc) {\n'
     '\t\tuint64_t ns;\n'
     '\t\tabsolutetime_to_nanoseconds(mach_absolute_time(), &ns);\n'
     '\t\t*secs = (clock_sec_t)(s8rtc + ns / NSEC_PER_SEC);\n'
     '\t\tnsecs = (clock_nsec_t)(ns % NSEC_PER_SEC);\n'
     '\t}\n'
     '#endif\n\n\tassert(nsecs < NSEC_PER_SEC);\n', 1),
    ('#include <pexpert/pexpert.h>\n#include <uuid/uuid.h>\n',
     '#include <pexpert/pexpert.h>\n#include <uuid/uuid.h>\n#include <kern/clock.h>\n'
     '#include <mach/mach_time.h>\n', 1),
])

# Present at SwapEnd. Boot 29 (sweetfb_hz=30) showed heavy artifacts: the
# vsync timer copied the latest swapped surface whenever it fired, while
# CoreAnimation (SwapWait is a no-op) could already be drawing into it.
# During the SwapEnd call the surface is stable, so copy there, at most once
# per 1/hz; the timer only flushes a swap that was skipped. One lock
# serializes the two copy paths.
edit('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ('\tuint32_t      fFadeStep;      /* fade-out steps done while off */\n',
     '\tuint32_t      fFadeStep;      /* fade-out steps done while off */\n'
     '\tIOLock       *fPresentLock;   /* one panel copy at a time */\n'
     '\tuint64_t      fLastPresent;   /* mach_absolute_time of the last copy */\n'
     '\tuint64_t      fMinGap;        /* 1/hz in timebase ticks */\n', 1),
    ('\tfFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;\n',
     '\tfFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;\n'
     '\tfMinGap = 26000000ULL / (hz == 0 ? 60 : hz);     /* 26 MHz timebase */\n'
     '\tfPresentLock = IOLockAlloc();\n'
     '\tif (fPresentLock == NULL) {\n'
     '\t\treturn false;\n'
     '\t}\n', 1),
    ('\tmemcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);\n\tfDirty = true;\n\tIOLockUnlock(fLock);\n',
     '\tmemcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);\n\tfDirty = true;\n'
     '\tuint64_t now = mach_absolute_time();\n'
     '\tbool copyNow = !fPowerOff && now - fLastPresent >= fMinGap;\n'
     '\tif (copyNow) {\n'
     '\t\tfDirty = false;\n'
     '\t\tfLastPresent = now;\n'
     '\t}\n'
     '\tIOLockUnlock(fLock);\n'
     '\tif (copyNow) {\n'
     '\t\tpresent(rec);\n'
     '\t}\n', 1),
    ('\tuint32_t mask = get32(rec, 0x104);\n',
     '\tuint32_t mask = get32(rec, 0x104);\n\tIOLockLock(fPresentLock);\n'
     '#if defined(DREAMLTE)\n\tsweet_fb_wait_frame_done();\n#endif\n', 1),
    ('static uint32_t sweet_fb_swaps_logged;\n',
     '#if defined(DREAMLTE)\n'
     '/*\n'
     ' * Boot 31 video: torn frames (top half of one screen, bottom of the\n'
     ' * next). DECON_F fetches the panel memory at 60 Hz (boot 30 probe: 30\n'
     ' * frame-done events per 0.5 s) while we copy into it. Start each copy\n'
     ' * right after a frame-done (INTERRUPT_PENDING 0x4c bit 13, source\n'
     ' * enabled in INTERRUPT_ENABLE 0x40; the SPI is not routed), so the copy\n'
     ' * runs ahead of the next fetch.\n'
     ' */\n'
     'extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);\n'
     'static vm_offset_t sweet_fb_decon;\n'
     '\n'
     'static void\n'
     'sweet_fb_wait_frame_done(void)\n'
     '{\n'
     '\tif (sweet_fb_decon == 0) {\n'
     '\t\tsweet_fb_decon = ml_io_map(0x12860000, 0x1000);\n'
     '\t\tif (sweet_fb_decon == 0) {\n'
     '\t\t\treturn;\n'
     '\t\t}\n'
     '\t\t*(volatile uint32_t *)(sweet_fb_decon + 0x40) |= (1u << 13) | 1u;\n'
     '\t}\n'
     '\tvolatile uint32_t *pend = (volatile uint32_t *)(sweet_fb_decon + 0x4c);\n'
     '\t*pend = 1u << 13;\n'
     '\tfor (int i = 0; i < 200 && !(*pend & (1u << 13)); i++) {\n'
     '\t\tIODelay(100);\n'
     '\t}\n'
     '}\n'
     '#endif\n'
     '\n'
     'static uint32_t sweet_fb_swaps_logged;\n', 1),
    ('\t\ts->release();\n\t}\n}\n',
     '\t\ts->release();\n\t}\n\tIOLockUnlock(fPresentLock);\n}\n', 1),
])

# Battery: the A5XELTE config had no power source, so iOS showed a wrong
# battery. On the S8, SweetBattery reads the MAX77865 fuel gauge and charger
# through dreamlte_battery_read() (dreamlte_touch.c). S8 battery: 3000 mAh.
edit('iokit/Kernel/arm/SweetBattery.cpp', [
    ('#if defined(ARM64_BOARD_CONFIG_SWEET)\n#include <device/device_types.h>',
     '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '#include <device/device_types.h>', 1),
    ('/* bsd/dev/sweet_spmi.c */\n'
     'extern "C" int sweet_spmi_init(void);\n'
     'extern "C" int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);\n',
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '/* Galaxy S8: MAX77865 fuel gauge (bsd/dev/a5xelte_touch.c). */\n'
     'extern "C" int dreamlte_battery_read(unsigned *percent, unsigned *mv, int *ma,\n'
     '    int *external, int *charging, int *full);\n'
     'static inline int sweet_spmi_init(void) { return -1; }\n'
     'static inline int sweet_spmi_read(unsigned, unsigned, uint8_t *) { return -1; }\n'
     '#define SWEET_BAT_MAH 3000u\n'
     '#else\n'
     '/* bsd/dev/sweet_spmi.c */\n'
     'extern "C" int sweet_spmi_init(void);\n'
     'extern "C" int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);\n'
     '#define SWEET_BAT_MAH 5020u\n'
     '#endif\n', 1),
    ('5020u * percent', 'SWEET_BAT_MAH * percent', 1),
    ('setMaxCapacity(5020)', 'setMaxCapacity(SWEET_BAT_MAH)', 1),
    ('"AppleRawMaxCapacity", 5020)', '"AppleRawMaxCapacity", SWEET_BAT_MAH)', 1),
    ('"DesignCapacity", 5020)', '"DesignCapacity", SWEET_BAT_MAH)', 1),
    ('{capacity, 5020, mv', '{capacity, SWEET_BAT_MAH, mv', 1),
    ('void\nSweetBattery::poll(void)\n{\n',
     'void\nSweetBattery::poll(void)\n{\n'
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
     '\tunsigned pct = 0, s8mv = 0;\n'
     '\tint s8ma = 0, ext = 0, chg = 0, full = 0;\n'
     '\tif (fReal && dreamlte_battery_read(&pct, &s8mv, &s8ma, &ext, &chg, &full) == 0) {\n'
     '\t\tpublish(full ? 100 : pct, s8mv, s8ma, ext != 0, ext != 0, chg != 0, full != 0, true, true);\n'
     '\t} else {\n'
     '\t\tpublish(100, 4200, 0, true, true, false, true, false, false);\n'
     '\t}\n'
     '#else\n', 1),
    ('\t    charging, full, true, currentValid);\n}\n',
     '\t    charging, full, true, currentValid);\n#endif\n}\n', 1),
    ('\tint real = 0;\n\t(void)PE_parse_boot_argn("sweetbattery"',
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n\tint real = 1;\n#else\n\tint real = 0;\n#endif\n'
     '\t(void)PE_parse_boot_argn("sweetbattery"', 1),
    ('\tfReal = real == 1 && sweet_spmi_init() == 0;\n',
     '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n\tfReal = real == 1;\n#else\n'
     '\tfReal = real == 1 && sweet_spmi_init() == 0;\n#endif\n', 1),
    ('measured ? "PM6150 / estimated SOC" : "static fallback"',
     'measured ? "fuel gauge" : "static fallback"', 1),
])
edit('iokit/KernelConfigTables.cpp', [
    ('#if !defined(ARM64_BOARD_CONFIG_A5XELTE)\n    "   {"\n    "     \'IOClass\'         = SweetBattery;"',
     '#if !defined(ARM64_BOARD_CONFIG_A5XELTE) || 1  /* S8: MAX77865 */\n    "   {"\n'
     '    "     \'IOClass\'         = SweetBattery;"', 1),
])

# S8 S6SY761 driver (dreamlte_touch.c next to this script), started from
# the same IOKitBSDInit hook in place of the A5 probe.
edit('bsd/dev/a5xelte_touch.c', [
    ('void\na5xelte_touch_probe(void)\n{\n',
     (Path(__file__).parent / 'dreamlte_touch.c').read_text() +
     'void\na5xelte_touch_probe(void)\n{\n'
     '#if defined(DREAMLTE)\n\tdreamlte_touch_start();   /* the A5 path below stays off */\n#endif\n', 1),
])

# Secondary CPUs. Boots 19-21 started the A53s with a PMU power edge
# (CPUn_CONFIGURATION 0 -> 0xF around PSCI CPU_ON) and a low trampoline; the
# Mongoose cluster never came up that way. galaxios8 boot m8 (loader E2) found
# the cause: S-Boot leaves the GIC distributor disabled (GICD_CTLR = 0) and
# sets CPUn_OPTION = 0x10a, whose bit 8 (ENABLE_AUTO_WAKEUP_INT_REQ) makes the
# PMU power a core (and its cluster) up on a GIC wake request. With GICD
# enabled, plain PSCI CPU_ON starts both 0x101 and Mongoose 0x000, as in
# TWRP's Linux. The firmware keeps the CPU_ON entry in a 32-bit PMU INFORM
# register, so the low trampoline stays.
edit('iokit/Kernel/arm/SweetCPU.cpp', [
    ('#include <pexpert/pexpert.h>\n',
     '#include <pexpert/pexpert.h>\n'
     '#if defined(DREAMLTE)\n'
     'extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);\n'
     '#endif\n', 1),
    ('\tif (start_paddr != 0 || arg_paddr != 0 || cpu->phys_id == 0 ||\n',
     '\tif (start_paddr != 0 || arg_paddr != 0 || cpu->phys_id == A5_BOOT_MPIDR ||\n', 1),
    ('\t__builtin_arm_dsb(DSB_SY);\n\tint32_t rc = sweet_psci_cpu_on(cpu->phys_id, entry, getCPUNumber());\n',
     '#if defined(DREAMLTE)\n'
     '\t{\n'
     '\t\t/* ldr x1, 8; br x1; .quad entry, in the unused low bank */\n'
     '\t\tstatic vm_offset_t tramp, gicd;\n'
     '\t\tconst uint64_t tramp_pa = 0x8f000000ULL;\n'
     '\t\tif (tramp == 0) {\n'
     '\t\t\ttramp = ml_io_map(tramp_pa, 0x1000);\n'
     '\t\t\tgicd = ml_io_map(0x10201000, 0x1000);\n'
     '\t\t}\n'
     '\t\tvolatile uint32_t *t = (volatile uint32_t *)tramp;\n'
     '\t\tt[0] = 0x58000041;              /* ldr x1, #8 */\n'
     '\t\tt[1] = 0xd61f0020;              /* br x1 */\n'
     '\t\tt[2] = (uint32_t)entry;\n'
     '\t\tt[3] = (uint32_t)((uint64_t)entry >> 32);\n'
     '\t\tvolatile uint32_t *ctlr = (volatile uint32_t *)gicd;\n'
     '\t\tuint32_t was = *ctlr;\n'
     '\t\t*ctlr = was | 1;                /* GICD_CTLR: PMU auto wakeup */\n'
     '\t\t__builtin_arm_dsb(DSB_SY);\n'
     '\t\tIOLog(CPU_PORT_NAME " CPU: cpu=%u mpidr=0x%llx entry 0x%llx via 0x%llx, GICD_CTLR 0x%x\\n",\n'
     '\t\t    getCPUNumber(), (uint64_t)cpu->phys_id, (uint64_t)entry, tramp_pa, was);\n'
     '\t\tentry = (vm_offset_t)tramp_pa;\n'
     '\t}\n'
     '#endif\n'
     '\t__builtin_arm_dsb(DSB_SY);\n\tint32_t rc = sweet_psci_cpu_on(cpu->phys_id, entry, getCPUNumber());\n', 1),
])

# Test cycles without anyone at the phone: s8autoreset=N makes the kernel
# reset straight into TWRP N seconds after the UFS log starts, after giving
# the log thread time to flush (it writes once a second). Same registers as
# Samsung's sec_reboot.c: INFORM2 = POWER_RESET, INFORM3 = REASON_RECOVERY,
# PMU SWRESET. Off unless the boot-arg is given.
edit('bsd/dev/sweet_ufs.c', [
    ('static void\nsweet_log_start(void)\n{\n',
     'static uint32_t s8_autoreset_secs;\n'
     '\n'
     'static void\n'
     's8_autoreset_thread(__unused void *arg, __unused wait_result_t wr)\n'
     '{\n'
     '\tint chan;\n'
     '\n'
     '\t(void)tsleep(&chan, PZERO, "s8reset", (int)s8_autoreset_secs * hz);\n'
     '\tprintf("S8 AUTORESET: %u s, resetting to recovery after the log flush\\n",\n'
     '\t    s8_autoreset_secs);\n'
     '\t(void)tsleep(&chan, PZERO, "s8reset", 3 * hz);\n'
     '\tvolatile uint32_t *pmu = (volatile uint32_t *)ml_io_map(0x16480000, 0x1000);\n'
     '\tpmu[0x808 / 4] = 0x12345678;       /* INFORM2: SEC_POWER_RESET */\n'
     '\tpmu[0x80c / 4] = 0x12345674;       /* INFORM3: REASON_RECOVERY */\n'
     '\t__asm__ volatile ("dsb sy" ::: "memory");\n'
     '\tpmu[0x400 / 4] = 1;                /* SWRESET */\n'
     '\tfor (;;) {\n'
     '\t\t(void)tsleep(&chan, PZERO, "s8reset", hz);\n'
     '\t}\n'
     '}\n'
     '\n'
     'static void\nsweet_log_start(void)\n{\n'
     '\tthread_t reset_thread;\n'
     '\n'
     '\tif (PE_parse_boot_argn("s8autoreset", &s8_autoreset_secs, sizeof(s8_autoreset_secs)) &&\n'
     '\t    s8_autoreset_secs != 0 &&\n'
     '\t    kernel_thread_start(s8_autoreset_thread, NULL, &reset_thread) == KERN_SUCCESS) {\n'
     '\t\tthread_deallocate(reset_thread);\n'
     '\t}\n', 1),
])

edit('bsd/dev/a5xelte_keys.c', [
    ('#include <pexpert/pexpert.h>\n',
     '#include <pexpert/pexpert.h>\n#include <pexpert/arm64/board_config.h>\n', 1),
    ('#define A5_ALIVE_PA 0x139b0000u\n',
     '#if defined(DREAMLTE)\n'
     '#define A5_ALIVE_PA 0x164b0000u            /* Exynos 8895 pinctrl ALIVE */\n'
     '#else\n'
     '#define A5_ALIVE_PA 0x139b0000u\n'
     '#endif\n', 1),
    ('static void\na5_keys_wait(void)\n',
     '#if defined(DREAMLTE)\n'
     '/*\n'
     ' * Galaxy S8 (mainline exynos8895-dreamlte.dts gpio-keys, all active low):\n'
     ' * Power GPA2-4, Volume Up GPA0-3, Volume Down GPA0-4, Bixby GPA0-6 (used\n'
     ' * as Home). Bank offsets per mainline pinctrl-exynos-arm64.c\n'
     ' * (exynos8895_pin_banks0): GPA0 0x020, GPA2 0x060; DAT at +4. Boot 22/23\n'
     ' * read 0x044 (GPA1 DAT) and saw nothing. Sample = GPA0 | GPA2 << 8.\n'
     ' */\n'
     'typedef uint16_t keys_t;\n'
     '#define KEYS_SAMPLE() ((keys_t)((a5_keys_read(0x24) & 0xff) | (a5_keys_read(0x64) & 0xff) << 8))\n'
     '#define KEYS_PINS { 12, 3, 4, 6 }\n'
     '#else\n'
     'typedef uint8_t keys_t;\n'
     '#define KEYS_SAMPLE() ((keys_t)a5_keys_read(A5_GPA2_DAT))\n'
     '#define KEYS_PINS { 7, 0, 1, 5 }\n'
     '#endif\n'
     '\n'
     'static void\na5_keys_wait(void)\n', 1),
    ('\tstatic const uint8_t pins[4] = {7, 0, 1, 5};\n'
     '\tuint8_t stable = (uint8_t)a5_keys_read(A5_GPA2_DAT);\n'
     '\tuint8_t previous = stable;\n',
     '\tstatic const uint8_t pins[4] = KEYS_PINS;\n'
     '\tkeys_t stable = KEYS_SAMPLE();\n'
     '\tkeys_t previous = stable;\n', 1),
    ('\t\tuint8_t sample = (uint8_t)a5_keys_read(A5_GPA2_DAT);\n',
     '\t\tkeys_t sample = KEYS_SAMPLE();\n', 1),
    ('\t\t\t\tuint8_t bit = 1u << pins[key];\n',
     '\t\t\t\tkeys_t bit = (keys_t)(1u << pins[key]);\n', 1),
    ('\tint enabled = 1;\n\tif (a5_keys_started) {',
     '\tint enabled = A5_KEYS_DEFAULT;\n\tif (a5_keys_started) {', 1),
])
