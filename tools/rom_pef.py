#!/usr/bin/env python
"""Generic PEF walker for the flat Mac OS ROM.

The OHCI-specific tool (ohci_uim_disasm.py) only ever finds one container. This
one enumerates every "Joy!peff" in the image, names each from the resource
header that precedes it, and disassembles any of them by export name or by
code-section offset. Names in the ROM's header run one entry ahead of their
container, so both the string in front of a container and the one in front of
the *next* container are reported and the caller picks.

Usage:
  python tools/rom_pef.py <flat-rom> --list
  python tools/rom_pef.py <flat-rom> --exports <container-off>
  python tools/rom_pef.py <flat-rom> --imports <container-off>
  python tools/rom_pef.py <flat-rom> --disasm <container-off> <code-off> [count]
  python tools/rom_pef.py <flat-rom> --sym <container-off> <name> [count]
"""

import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from ohci_uim_disasm import Pef


def containers(data):
    return [m.start() for m in re.finditer(b"Joy!peff", data)]


def names_near(data, off):
    """Printable strings in the 0x100 bytes before a container."""
    back = data[max(0, off - 0x100):off]
    return re.findall(rb"[A-Za-z][A-Za-z0-9_,.+ -]{3,}", back)


def disasm(data, pef, code_off, count):
    import capstone
    code = pef.code()
    md = capstone.Cs(capstone.CS_ARCH_PPC,
                     capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    blob = data[code["coff"] + code_off:code["coff"] + code_off + count * 4]
    for ins in md.disasm(blob, code_off):
        print("%06x  %08x  %-8s %s"
              % (ins.address, struct.unpack(">I", ins.bytes)[0],
                 ins.mnemonic, ins.op_str))


def main():
    data = open(sys.argv[1], "rb").read()
    args = sys.argv[2:]

    if args[0] == "--list":
        for off in containers(data):
            pef = Pef(data, off)
            code = pef.code()
            print("%08x  code %08x+%06x  %s"
                  % (off, code["coff"] if code else 0,
                     code["clen"] if code else 0,
                     b" | ".join(names_near(data, off)[-3:]).decode("latin-1")))
        return

    off = int(args[1], 0)
    pef = Pef(data, off)

    if args[0] == "--exports":
        for name, sect, val, cls in pef.exports():
            print("  %-44s sect=%d off=0x%06x class=%d" % (name, sect, val, cls))
        return
    if args[0] == "--imports":
        libs, syms = pef.imports()
        for name, first, n in libs:
            print("  %s (%d)" % (name, n))
            for i, s in enumerate(syms[first:first + n]):
                print("    %2d %s" % (i, s))
        return
    if args[0] == "--disasm":
        disasm(data, pef, int(args[2], 0),
               int(args[3], 0) if len(args) > 3 else 64)
        return
    if args[0] == "--sym":
        want = args[2]
        count = int(args[3], 0) if len(args) > 3 else 64
        for name, sect, val, cls in pef.exports():
            if name == want:
                print("== %s sect=%d off=0x%x class=%d" % (name, sect, val, cls))
                if cls == 0:      # TVector: code offset is in the data section
                    print("   (TVector, disassembling code offset 0x%x)" % val)
                disasm(data, pef, val, count)
                return
        sys.exit("no export %r" % want)


if __name__ == "__main__":
    main()
