#!/usr/bin/env python3
"""Change the TITLE shown on the PS4 home screen: python3 set_title.py sce_sys/param.sfo "Jaja Video Player" """
import struct, sys
path, title = sys.argv[1], sys.argv[2]
d = bytearray(open(path, 'rb').read())
assert d[:4] == b'\0PSF', 'not a param.sfo'
kt, dt, n = struct.unpack_from('<III', d, 8)
for i in range(n):
    ko, fmt, ln, mx, do = struct.unpack_from('<HHIII', d, 20 + 16*i)
    key = d[kt+ko:d.index(b'\0', kt+ko)].decode()
    if key in ('TITLE',):
        t = title.encode() + b'\0'
        assert len(t) <= mx, 'title too long (max %d)' % mx
        d[dt+do:dt+do+mx] = t + b'\0' * (mx - len(t))
        struct.pack_into('<I', d, 20 + 16*i + 4, len(t))
        print('TITLE set to', title)
open(path, 'wb').write(d)
