#!/usr/bin/env python
"""Decode a NewWorld Mac OS ROM into the flat 4 MB image SheepShaver runs.

Mirrors DecodeROM()/decode_parcels()/decode_lzss() in SheepShaver's
rom_patches.cpp, so an address seen at run time as ROMBase+off is just off in
the output. Without this every ROM question needs a live emulator.

Usage: python tools/rom_decode.py "Mac OS ROM" rom_flat.bin
"""

import re
import struct
import sys

ROM_SIZE = 0x400000


def decode_lzss(src, dest, dest_off, size):
    dict_buf = bytearray(0x1000)
    run_mask = 0
    dict_idx = 0xfee
    i = 0
    while True:
        if run_mask < 0x100:
            size -= 1
            if size < 0:
                break
            run_mask = src[i] | 0xff00
            i += 1
        bit = run_mask & 1
        run_mask >>= 1
        if bit:
            size -= 1
            if size < 0:
                break
            c = src[i]
            i += 1
            dict_buf[dict_idx] = c
            dest[dest_off] = c
            dest_off += 1
            dict_idx = (dict_idx + 1) & 0xfff
        else:
            size -= 1
            if size < 0:
                break
            idx = src[i]
            i += 1
            size -= 1
            if size < 0:
                break
            cnt = src[i]
            i += 1
            idx |= (cnt << 4) & 0xf00
            cnt = (cnt & 0x0f) + 3
            while cnt:
                cnt -= 1
                c = dict_buf[idx]
                dict_buf[dict_idx] = c
                dest[dest_off] = c
                dest_off += 1
                idx = (idx + 1) & 0xfff
                dict_idx = (dict_idx + 1) & 0xfff
    return dest_off


def decode_parcels(src, base, dest):
    off = 0x14
    while off != 0:
        nxt, ptype = struct.unpack_from(">II", src, off)
        name = src[off + 24:off + 56].split(b"\0")[0].decode("latin-1")
        sys.stderr.write("%08x %s %s\n" % (off, struct.pack(">I", ptype).decode("latin-1"), name))
        if ptype == 0x726f6d20:                 # 'rom '
            lzss_off = struct.unpack_from(">I", src, off + 8)[0]
            lzss_size = nxt - (off + lzss_off)
            decode_lzss(src[off + lzss_off:], dest, 0, lzss_size)
        off = nxt


def main():
    data = open(sys.argv[1], "rb").read()
    out = bytearray(ROM_SIZE)
    if len(data) == ROM_SIZE:
        out[:] = data
    elif data[:11] == b"<CHRP-BOOT>":
        txt = data[:0x10000]
        def num(tag):
            m = re.search((r"([0-9a-fA-F]{6}) constant " + tag).encode(), txt)
            return int(m.group(1), 16) if m else None
        off = num("parcels-offset")
        if off is not None:
            decode_parcels(data[off:], off, out)
        else:
            off = num("lzss-offset")
            size = num("lzss-size")
            decode_lzss(data[off:], out, 0, size)
    else:
        sys.exit("unrecognised ROM image")
    open(sys.argv[2], "wb").write(bytes(out))
    sys.stderr.write("wrote %s (%d bytes)\n" % (sys.argv[2], len(out)))


if __name__ == "__main__":
    main()
