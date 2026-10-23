#!/usr/bin/env python3
"""Build a drop-zone image for tools/dreamlte/s8drop.c (see there for the format).

  mkdrop.py OUT.img [+]FILE...      (+ = run it at boot, in the order given)
"""
import os
import struct
import sys

out, args = sys.argv[1], sys.argv[2:]
if not args:
    sys.exit(__doc__)
entries, blobs, offset = [], [], 4096
for a in args:
    run = a.startswith('+')
    path = a[1:] if run else a
    data = open(path, 'rb').read()
    name = os.path.basename(path).encode()
    assert len(name) < 48 and b'/' not in name
    entries.append(struct.pack('<48sIIQQ', name, 1 if run else 0, 0, offset, len(data)))
    blobs.append((offset, data))
    offset = (offset + len(data) + 4095) & ~4095
assert 16 + 72 * len(entries) <= 4096
hdr = b'S8DROP01' + struct.pack('<II', len(entries), 0) + b''.join(entries)
img = bytearray(offset)
img[:len(hdr)] = hdr
for off, data in blobs:
    img[off:off + len(data)] = data
open(out, 'wb').write(img)
print(f'{out}: {len(entries)} files, {len(img)} bytes')
