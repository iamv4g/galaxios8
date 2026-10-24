#!/usr/bin/env python3
"""Read the screenshots tools/dreamlte/s8shot.c wrote to USERDATA (21 GiB + k * 16 MiB) from the
Galaxy S8 in TWRP and save them as PNG in logs/ (shot-NAME-K.png). Reads only, the S8 only.

  tools/dreamlte/pull-shot.py NAME
"""
import os, struct, subprocess, sys, zlib

SERIAL = 'ce081718edd2030d02'
name = sys.argv[1]
env = dict(os.environ, ANDROID_SERIAL=SERIAL)
adb = lambda *a, **k: subprocess.run(['adb', *a], env=env, check=True, capture_output=True, **k).stdout
assert adb('shell', 'getprop', 'ro.product.device').strip() == b'dreamlte'
logs = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../logs')


def png(path, w, h, rows):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


for k in range(8):
    base = (21 << 30) + k * (16 << 20)
    hdr = adb('exec-out', f'dd if=/dev/block/sda24 bs=4096 skip={base // 4096} count=1 2>/dev/null')
    if hdr[:8] != b'S8SHOT01':
        break
    sid, w, h, bpr, fmt, up, idx, _ = struct.unpack('<8I', hdr[8:40])
    n = (h * bpr + 4095) // 4096
    data = adb('exec-out', f'dd if=/dev/block/sda24 bs=4096 skip={base // 4096 + 1} count={n} 2>/dev/null')
    rows = []
    for y in range(h):
        r = data[y * bpr:y * bpr + w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = r[2::4], r[1::4], r[0::4]     # BGRA -> RGB
        rows.append(bytes(rgb))
    out = os.path.join(logs, f'shot-{name}-{k}.png')
    png(out, w, h, rows)
    print(f'{out}: surface {sid} {w}x{h} at {up} s after boot')
