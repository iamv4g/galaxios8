#!/usr/bin/env python3
"""Make Mesa 25.0.7's Panfrost build and run on iOS on the Galaxy S8 (S8Mali kernel driver).

  patch-mesa.py MESA_TREE

- libdrm: on Darwin with panfrost, dep_libdrm is tools/dreamlte/mesa/s8drm (copied to
  MESA_TREE/src/s8drm by mk-mesa.sh), built into the panfrost kmod library.
- CPU mappings of buffers: client memory of the S8Mali user client (s8_drm_mmap/munmap) in
  place of mmap() on a DRM file descriptor.
- OSMesa: its screen is Panfrost's when the kernel has the S8Mali service and the environment
  does not say OSMESA_SOFTWARE=1; otherwise softpipe, as before.
- IOSurfaces as GL images: glEGLImageTargetTexture2DOES(target, (GLeglImageOES)surfaceID)
  makes the surface's memory a texture (and, attached to an FBO, a render target), as
  HackiOS's freedreno-darwin.patch (MIT) does for freedreno. The surface comes in where Linux
  passes a dma-buf fd: s8drm's drmPrimeFDToHandle imports it (S8Mali selector 8).
Every replacement asserts its match count, as tools/dreamlte/apply-dreamlte.py does.
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


edit('meson.build', [(
    "  dep_libdrm = null_dep\nendif\nif dep_libdrm.found()\n",
    "  dep_libdrm = null_dep\nendif\n"
    "if host_machine.system() == 'darwin' and with_gallium_panfrost\n"
    "  # No DRM on iOS: Panfrost's requests go to the S8Mali kernel driver through a\n"
    "  # libdrm stand-in (src/s8drm, from galaxios8 tools/dreamlte/mesa/s8drm).\n"
    "  dep_libdrm = declare_dependency(\n"
    "    include_directories : include_directories('src/s8drm'),\n"
    "    link_args : ['-framework', 'IOKit', '-framework', 'IOSurface', '-framework', 'CoreFoundation'],\n"
    "  )\n"
    "elif dep_libdrm.found()\n", 1)])

edit('src/panfrost/lib/kmod/meson.build', [(
    "  'panthor_kmod.c',\n)\n",
    "  'panthor_kmod.c',\n)\n"
    "if host_machine.system() == 'darwin'\n"
    "  libpankmod_lib_files += files('../../../s8drm/s8_drm.c')\n"
    "endif\n", 1)])

edit('src/panfrost/lib/kmod/pan_kmod.h', [(
    "   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,\n"
    "                       mmap_offset + bo_offset);\n",
    "#ifdef __APPLE__\n"
    "   (void)prot;\n"
    "   (void)flags;\n"
    "   host_addr = s8_drm_mmap(bo->dev->fd, mmap_offset, size);\n"
    "   if (host_addr != MAP_FAILED)\n"
    "      host_addr = (char *)host_addr + bo_offset;\n"
    "#else\n"
    "   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,\n"
    "                       mmap_offset + bo_offset);\n"
    "#endif\n", 1)])

edit('src/gallium/drivers/panfrost/pan_bo.c', [(
    "   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n",
    "#ifdef __APPLE__\n"
    "   if (s8_drm_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n"
    "#else\n"
    "   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n"
    "#endif\n", 1)])

edit('src/gallium/targets/osmesa/meson.build', [(
    "    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,\n",
    "    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,\n"
    "    driver_panfrost, idep_xmlconfig, dep_libdrm,\n", 1)])

edit('src/gallium/targets/osmesa/osmesa_target.c', [
    ('#include "sw/null/null_sw_winsys.h"\n',
     '#include "sw/null/null_sw_winsys.h"\n'
     '\n'
     '#ifdef GALLIUM_PANFROST\n'
     '#include "panfrost/drm/panfrost_drm_public.h"\n'
     '#include "util/u_debug.h"\n'
     '#include "util/driconf.h"\n'
     '#include "util/xmlconfig.h"\n'
     '#include <xf86drm.h>\n'
     '\n'
     'static const driOptionDescription panfrost_options[] = {\n'
     '#include "panfrost/driinfo_panfrost.h"\n'
     '};\n'
     '\n'
     '/* The Mali-G71 through the S8Mali kernel driver (iOS on the Galaxy S8); NULL: none. */\n'
     'static struct pipe_screen *\n'
     'osmesa_create_gpu_screen(void)\n'
     '{\n'
     '   static driOptionCache info, cache;\n'
     '   static struct pipe_screen_config config;\n'
     '   struct pipe_screen *screen;\n'
     '   int fd;\n'
     '\n'
     '   if (debug_get_bool_option("OSMESA_SOFTWARE", false))\n'
     '      return NULL;\n'
     '   fd = s8_drm_open();\n'
     '   if (fd < 0)\n'
     '      return NULL;\n'
     '   driParseOptionInfo(&info, panfrost_options, ARRAY_SIZE(panfrost_options));\n'
     '   driParseConfigFiles(&cache, &info, 0, "panfrost", NULL, NULL, NULL, 0, NULL, 0);\n'
     '   config.options = &cache;\n'
     '   config.options_info = &info;\n'
     '   screen = panfrost_drm_screen_create(fd, &config);\n'
     '   s8_drm_report(0x51, screen != NULL, 0);\n'
     '   return screen ? debug_screen_wrap(screen) : NULL;\n'
     '}\n'
     '#endif\n', 1),
    ('   struct sw_winsys *winsys;\n   struct pipe_screen *screen;\n\n',
     '   struct sw_winsys *winsys;\n   struct pipe_screen *screen;\n\n'
     '#ifdef GALLIUM_PANFROST\n'
     '   screen = osmesa_create_gpu_screen();\n'
     '   if (screen)\n'
     '      return screen;\n'
     '#endif\n\n', 1),
])

edit('src/panfrost/lib/kmod/pan_kmod.c', [(
    "      size_t size = lseek(fd, 0, SEEK_END);\n",
    "#ifdef __APPLE__\n"
    "      size_t size = s8_drm_import_size(handle);     /* fd: an IOSurface ID */\n"
    "#else\n"
    "      size_t size = lseek(fd, 0, SEEK_END);\n"
    "#endif\n", 1)])

edit('src/gallium/frontends/osmesa/osmesa.c', [
    ("extern struct pipe_screen *\nosmesa_create_screen(void);\n",
     "extern struct pipe_screen *\nosmesa_create_screen(void);\n\n"
     "extern struct pipe_resource *\nosmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);\n", 1),
    ("static void\ncreate_st_manager(void)\n",
     "/* No EGL here: the image of glEGLImageTargetTexture2DOES is an IOSurface ID. */\n"
     "static bool\n"
     "osmesa_st_get_egl_image(struct pipe_frontend_screen *fscreen, void *image,\n"
     "                        struct st_egl_image *out)\n"
     "{\n"
     "   memset(out, 0, sizeof(*out));\n"
     "   out->texture = osmesa_surface_resource(fscreen->screen, (uint32_t)(uintptr_t)image);\n"
     "   if (!out->texture)\n"
     "      return false;\n"
     "   out->format = out->texture->format;\n"
     "   return true;\n"
     "}\n\n"
     "static bool\n"
     "osmesa_st_validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)\n"
     "{\n"
     "   return image != NULL;\n"
     "}\n\n"
     "static void\ncreate_st_manager(void)\n", 1),
    ("      global_fscreen->get_egl_image = NULL;\n",
     "      global_fscreen->get_egl_image = osmesa_st_get_egl_image;\n"
     "      global_fscreen->validate_egl_image = osmesa_st_validate_egl_image;\n", 1),
])

edit('src/gallium/targets/osmesa/osmesa_target.c', [(
    "struct pipe_screen *\nosmesa_create_screen(void);\n",
    "struct pipe_resource *\nosmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);\n\n"
    "/*\n"
    " * An IOSurface's memory as a texture (sampler view and render target): one plane of 32-bit\n"
    " * BGRA, linear, imported by the kernel driver. NULL: not the GPU's screen, or another format.\n"
    " */\n"
    "struct pipe_resource *\n"
    "osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface)\n"
    "{\n"
    "#if defined(GALLIUM_PANFROST) && defined(__APPLE__)\n"
    "   struct s8_drm_surface s;\n"
    "   if (s8_drm_surface_info(surface, &s) != 0 || s.pixel_format != 0x42475241 /* 'BGRA' */)\n"
    "      return NULL;\n"
    "   struct pipe_resource templ = {\n"
    "      .target = PIPE_TEXTURE_2D,\n"
    "      .format = PIPE_FORMAT_B8G8R8A8_UNORM,\n"
    "      .width0 = s.width,\n"
    "      .height0 = s.height,\n"
    "      .depth0 = 1,\n"
    "      .array_size = 1,\n"
    "      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,\n"
    "   };\n"
    "   struct winsys_handle handle = {\n"
    "      .type = WINSYS_HANDLE_TYPE_FD,\n"
    "      .handle = surface,\n"
    "      .stride = s.bytes_per_row,\n"
    "      .offset = 0,\n"
    "      .size = s.size,\n"
    "      .modifier = DRM_FORMAT_MOD_LINEAR,\n"
    "   };\n"
    "   return screen->resource_from_handle(screen, &templ, &handle, 0);\n"
    "#else\n"
    "   return NULL;\n"
    "#endif\n"
    "}\n\n"
    "struct pipe_screen *\nosmesa_create_screen(void);\n", 1),
    ('#include "util/xmlconfig.h"\n',
     '#include "util/xmlconfig.h"\n#include "frontend/winsys_handle.h"\n#include "pipe/p_screen.h"\n#include "pipe/p_state.h"\n#include "drm-uapi/drm_fourcc.h"\n', 1),
])
