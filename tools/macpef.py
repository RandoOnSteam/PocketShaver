#!/usr/bin/env python
"""MacBinary / resource fork / PEF inspector and disassembler.

One tool for the thing this repo keeps doing: take a classic Mac file, get at
its forks, find the PEF fragments and 68k resources inside, and disassemble
them with imports resolved to names.

  python tools/macpef.py info    "InputSprocket Extension.bin"
  python tools/macpef.py forks   "Descent II.bin" --out /tmp/d2
  python tools/macpef.py rsrc    "Descent II.bin"
  python tools/macpef.py rsrc    "Firebird INIT.bin" --type INIT --out init.bin
  python tools/macpef.py pef     "InputSprocket Extension.bin"
  python tools/macpef.py imports "InputSprocket Extension.bin" -c 0x4fe40
  python tools/macpef.py exports "InputSprocket Extension.bin" -c 0
  python tools/macpef.py toc     "InputSprocket Extension.bin" -c 0x4fe40
  python tools/macpef.py dis     "InputSprocket Extension.bin" -c 0x4fe40 -a 0x361c -n 80
  python tools/macpef.py dis     "Firebird INIT.bin" --rsrc ADBS -a 0 --arch 68k
  python tools/macpef.py data    "InputSprocket Extension.bin" -c 0x4fe40 -a 0x388 -n 80

See docs/macpef.md.
"""

import argparse
import re
import struct
import sys


# ---------------------------------------------------------------- MacBinary

def split_forks(blob):
    """(data, rsrc). A MacBinary wrapper if there is one, else the whole file."""
    if len(blob) >= 128 and blob[0] == 0 and blob[74] == 0 and blob[82] == 0:
        dlen, rlen = struct.unpack_from(">II", blob, 83)
        if 128 + dlen + rlen <= len(blob) + 128:
            data = blob[128:128 + dlen]
            roff = 128 + (dlen + 127) // 128 * 128
            return data, blob[roff:roff + rlen]
    return blob, b""


def macbinary_header(blob):
    if len(blob) < 128 or blob[0] != 0 or blob[74] != 0:
        return None
    n = blob[1]
    return dict(name=blob[2:2 + n].decode("mac-roman", "replace"),
                type=blob[65:69].decode("mac-roman", "replace"),
                creator=blob[69:73].decode("mac-roman", "replace"),
                data_len=struct.unpack_from(">I", blob, 83)[0],
                rsrc_len=struct.unpack_from(">I", blob, 87)[0])


# ------------------------------------------------------------ resource fork

def resources(rsrc):
    """Yield (type, id, name, offset, length) for every resource."""
    if len(rsrc) < 16:
        return
    data_off, map_off, data_len, map_len = struct.unpack_from(">4I", rsrc, 0)
    if map_off + 30 > len(rsrc):
        return
    type_off = struct.unpack_from(">H", rsrc, map_off + 24)[0] + map_off
    name_off = struct.unpack_from(">H", rsrc, map_off + 26)[0] + map_off
    ntypes = struct.unpack_from(">H", rsrc, type_off)[0] + 1
    for i in range(ntypes):
        e = type_off + 2 + i * 8
        if e + 8 > len(rsrc):
            return
        rtype = rsrc[e:e + 4].decode("mac-roman", "replace")
        count = struct.unpack_from(">H", rsrc, e + 4)[0] + 1
        refs = struct.unpack_from(">H", rsrc, e + 6)[0] + type_off
        for j in range(count):
            r = refs + j * 12
            if r + 12 > len(rsrc):
                return
            rid, noff = struct.unpack_from(">hh", rsrc, r)
            attr_off = struct.unpack_from(">I", rsrc, r + 4)[0]
            off = data_off + (attr_off & 0xFFFFFF)
            if off + 4 > len(rsrc):
                continue
            rlen = struct.unpack_from(">I", rsrc, off)[0]
            name = ""
            if noff >= 0 and name_off + noff < len(rsrc):
                ln = rsrc[name_off + noff]
                name = rsrc[name_off + noff + 1:name_off + noff + 1 + ln] \
                    .decode("mac-roman", "replace")
            yield rtype, rid, name, off + 4, rlen


def resource_blob(rsrc, want_type, want_id):
    for rtype, rid, _name, off, rlen in resources(rsrc):
        if rtype == want_type and (want_id is None or rid == want_id):
            return rsrc[off:off + rlen]
    return None


# --------------------------------------------------------------------- PEF

def varint(b, i):
    v = 0
    while True:
        c = b[i]
        i += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, i


def unpack_pidata(b, total):
    """Expand a PEF pattern-initialised (kind 2) section."""
    out = bytearray()
    i = 0
    while i < len(b) and len(out) < total:
        op = b[i]
        i += 1
        kind, count = op >> 5, op & 0x1F
        if count == 0:
            count, i = varint(b, i)
        if kind == 0:                                   # Zero
            out += b"\0" * count
        elif kind == 1:                                 # blockCopy
            out += b[i:i + count]
            i += count
        elif kind == 2:                                 # repeatedBlock
            times, i = varint(b, i)
            blk = b[i:i + count]
            i += count
            out += blk * (times + 1)
        elif kind == 3:                                 # interleave w/ copy
            custom, i = varint(b, i)
            times, i = varint(b, i)
            common = b[i:i + count]
            i += count
            for _ in range(times):
                out += common + b[i:i + custom]
                i += custom
            out += common
        elif kind == 4:                                 # interleave w/ zero
            custom, i = varint(b, i)
            times, i = varint(b, i)
            for _ in range(times):
                out += b"\0" * count + b[i:i + custom]
                i += custom
            out += b"\0" * count
        else:
            raise ValueError("bad pidata opcode %d" % kind)
    return bytes(out)


def _run_relocs(words, syms):
    """Execute a PEF relocation opcode stream; [(data offset, what)]."""
    out = []
    pos = imp = i = 0

    def note(what, n=1, width=4):
        for _ in range(n):
            out.append((pos_box[0], what))
            pos_box[0] += width

    pos_box = [0]

    def sym(n):
        return "import[%d] %s" % (n, syms[n] if n < len(syms) else "?")

    while i < len(words):
        op = words[i]
        i += 1
        if op >> 14 == 0:                               # BySectDWithSkip
            pos_box[0] += ((op >> 6) & 0xFF) * 4
            note("sectD", op & 0x3F)
        elif op >> 12 == 0x4:
            sub, n = (op >> 9) & 7, (op & 0x1FF) + 1
            if sub == 0:
                note("sectC", n)
            elif sub == 1:
                note("sectD", n)
            elif sub == 2:
                note("tvector12", n, 12)
            elif sub == 3:
                note("tvector8", n, 8)
            elif sub == 4:
                note("vtable8", n, 8)
            elif sub == 5:
                for _ in range(n):
                    out.append((pos_box[0], sym(imp)))
                    imp += 1
                    pos_box[0] += 4
            else:
                out.append((pos_box[0], "op%04x" % op))
        elif op >> 12 == 0x5:
            sub, idx = (op >> 9) & 7, op & 0x1FF
            if sub == 0:                                # SmByImport
                out.append((pos_box[0], sym(idx)))
                imp = idx + 1
                pos_box[0] += 4
            elif sub == 3:
                out.append((pos_box[0], "section[%d]" % idx))
                pos_box[0] += 4
            elif sub not in (1, 2):                     # 1/2 set sectC/sectD
                out.append((pos_box[0], "op%04x" % op))
        elif op >> 12 == 0x6:
            sub = (op >> 9) & 7
            if sub == 0:                                # IncrPosition
                pos_box[0] += (op & 0x1FF) + 1
            elif sub == 1:                              # SmRepeat
                blk = ((op >> 4) & 0x1F) + 1
                rpt = (op & 0xF) + 1
                chunk = words[i - 1 - blk:i - 1]
                for _ in range(rpt):
                    inner = _run_relocs(chunk, syms)
                    for off, what in inner:
                        out.append((pos_box[0] + off, what))
                    pos_box[0] += sum(4 for _ in inner)
            else:
                out.append((pos_box[0], "op%04x" % op))
        elif op >> 12 == 0x7:
            w2 = words[i]
            i += 1
            big = ((op & 0x3FF) << 16) | w2
            if (op >> 10) & 1:                          # LgByImport
                out.append((pos_box[0], sym(big)))
                imp = big + 1
                pos_box[0] += 4
            else:                                       # SetPosition
                pos_box[0] = big
        else:
            out.append((pos_box[0], "op%04x" % op))
    return out


class Pef(object):
    """One "Joy!peff" container inside a larger blob."""

    def __init__(self, blob, off):
        if blob[off:off + 8] != b"Joy!peff":
            raise ValueError("no PEF container at %#x" % off)
        self.blob = blob
        self.off = off
        count = struct.unpack_from(">H", blob, off + 32)[0]
        self.sections = []
        for i in range(count):
            s = off + 40 + i * 28
            name_off, addr, total, unpacked, clen, coff = \
                struct.unpack_from(">6I", blob, s)
            kind = blob[s + 24]
            self.sections.append(dict(index=i, name_off=name_off, addr=addr,
                                      total=total, unpacked=unpacked,
                                      clen=clen, coff=off + coff, kind=kind))
        self._data = None
        self._toc = None

    # -- sections ---------------------------------------------------------
    def section(self, kind):
        for s in self.sections:
            if s["kind"] == kind:
                return s
        return None

    def code(self):
        s = self.section(0)
        return self.blob[s["coff"]:s["coff"] + s["clen"]] if s else b""

    def data(self):
        """The data section, pattern-expanded and zero-padded to total."""
        if self._data is None:
            s = self.section(1) or self.section(2)
            if s is None:
                self._data = b""
            else:
                raw = self.blob[s["coff"]:s["coff"] + s["clen"]]
                out = unpack_pidata(raw, s["unpacked"]) if s["kind"] == 2 else raw
                self._data = out + b"\0" * max(0, s["total"] - len(out))
        return self._data

    # -- loader -----------------------------------------------------------
    def _loader(self):
        s = self.section(4)
        if s is None:
            return None, None
        b = self.blob[s["coff"]:s["coff"] + s["clen"]]
        return b, struct.unpack_from(">iIiIiI8I", b, 0)

    def imports(self):
        """([(library, first, count)], [symbol names])."""
        b, h = self._loader()
        if b is None:
            return [], []
        lib_count, sym_count, _rs, _ri, str_off = h[6], h[7], h[8], h[9], h[10]
        libs = []
        for i in range(lib_count):
            noff, _old, _init, cnt, first = \
                struct.unpack_from(">5I", b, 56 + i * 24)
            end = b.index(b"\0", str_off + noff)
            libs.append((b[str_off + noff:end].decode("mac-roman"), first, cnt))
        syms = []
        base = 56 + lib_count * 24
        for i in range(sym_count):
            v = struct.unpack_from(">I", b, base + i * 4)[0]
            end = b.index(b"\0", str_off + (v & 0xFFFFFF))
            syms.append(b[str_off + (v & 0xFFFFFF):end].decode("mac-roman"))
        return libs, syms

    def exports(self):
        """[(name, section, value, class)] - names are length-counted."""
        b, h = self._loader()
        if b is None:
            return []
        str_off, hash_off, hash_pow, n = h[10], h[11], h[12], h[13]
        key_off = hash_off + (1 << hash_pow) * 4
        sym_off = key_off + n * 4
        out = []
        for i in range(n):
            cls_name, value, sect = struct.unpack_from(">IIh", b, sym_off + i * 10)
            klen = struct.unpack_from(">I", b, key_off + i * 4)[0] >> 16
            s = str_off + (cls_name & 0xFFFFFF)
            out.append((b[s:s + klen].decode("mac-roman"), sect, value,
                        cls_name >> 24))
        return out

    def relocations(self):
        """{data offset: description} after running the relocation stream.

        The opcode table is the PEF one; this is a port of tools/pef_reloc.py,
        which is the version that has actually been checked against a fragment
        whose imports are known.
        """
        b, h = self._loader()
        if b is None:
            return {}
        lib_count, sym_count, sect_count, inst_off = h[6], h[7], h[8], h[9]
        _libs, syms = self.imports()
        ds = self.section(1) or self.section(2)
        if ds is None:
            return {}
        hdr = 56 + lib_count * 24 + sym_count * 4
        out = {}
        for i in range(sect_count):
            sect, _pad, count, first = struct.unpack_from(">HHII", b, hdr + i * 12)
            if sect != ds["index"]:
                continue
            words = struct.unpack_from(">%dH" % count, b, inst_off + first)
            for pos, what in _run_relocs(words, syms):
                out[pos] = what
        return out

    # -- TOC --------------------------------------------------------------
    def toc(self):
        """Where r2 points, as an offset into the expanded data section.

        PEF does not record it. Every import has a glue stub
        "lwz r12,d(r2); stw r2,20(r1)" and the relocation stream puts
        import[0] at data offset 0, so -min(d) is the answer.
        """
        if self._toc is None:
            code = self.code()
            lo = None
            for i in range(0, len(code) - 4, 4):
                w = struct.unpack_from(">I", code, i)[0]
                if w >> 16 == 0x8182 and \
                        struct.unpack_from(">I", code, i + 4)[0] == 0x90410014:
                    d = w & 0xFFFF
                    if d >= 0x8000:
                        d -= 0x10000
                    lo = d if lo is None or d < lo else lo
            self._toc = -lo if lo is not None else 0
        return self._toc

    def glue(self):
        """{code offset of an import glue stub: import name}."""
        rel = self.relocations()
        code = self.code()
        toc = self.toc()
        out = {}
        for i in range(0, len(code) - 4, 4):
            w = struct.unpack_from(">I", code, i)[0]
            if w >> 16 == 0x8182 and \
                    struct.unpack_from(">I", code, i + 4)[0] == 0x90410014:
                d = w & 0xFFFF
                if d >= 0x8000:
                    d -= 0x10000
                name = rel.get(toc + d)
                if name:
                    out[i] = name
        return out


def containers(blob):
    return [m.start() for m in re.finditer(b"Joy!peff", blob)]


def names_near(blob, off, back=0x100):
    """Printable strings just before a container - the ROM names them there."""
    chunk = blob[max(0, off - back):off]
    return [s.decode("mac-roman", "replace")
            for s in re.findall(rb"[A-Za-z][A-Za-z0-9_,.+ -]{3,}", chunk)]


# ------------------------------------------------------------- disassembly

def disassemble(blob, start, count, arch, pef=None, out=sys.stdout):
    try:
        import capstone
    except ImportError:
        sys.exit("capstone is not installed: pip install capstone")
    if arch == "68k":
        md = capstone.Cs(capstone.CS_ARCH_M68K,
                         capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_040)
        step = None
    else:
        md = capstone.Cs(capstone.CS_ARCH_PPC,
                         capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        step = 4
    glue = pef.glue() if pef is not None else {}
    toc = pef.toc() if pef is not None else 0
    data = pef.data() if pef is not None else b""
    rel = pef.relocations() if pef is not None else {}

    i = start
    end = start + count * (step or 6)
    shown = 0
    while i < min(end, len(blob)) and shown < count:
        got = list(md.disasm(blob[i:i + 16], i, 1))
        if not got:
            w = struct.unpack_from(">%s" % ("I" if step else "H"), blob, i)[0]
            out.write("%06x  %0*x  %-8s %s\n"
                      % (i, 8 if step else 4, w, ".long" if step else ".word",
                         "%#x" % w))
            i += step or 2
            shown += 1
            continue
        ins = got[0]
        note = ""
        if step:                                        # PowerPC annotations
            if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 0)
                if t in glue:
                    note = "   ; -> %s" % glue[t]
            if "(r2)" in ins.op_str:
                try:
                    d = ins.op_str.rsplit(",", 1)[-1].strip()
                    d = int(d[:d.index("(")], 0)
                    who = rel.get(toc + d)
                    if who:
                        note = "   ; %s" % who
                    elif toc + d + 4 <= len(data):
                        note = "   ; data+%#x = %08x" % (
                            toc + d,
                            struct.unpack_from(">I", data, toc + d)[0])
                except Exception:
                    pass
        out.write("%06x  %-12s %-8s %-30s%s\n"
                  % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, note))
        i += ins.size
        shown += 1


def hexdump(blob, start, length, out=sys.stdout):
    for a in range(start, min(start + length, len(blob)), 16):
        row = blob[a:a + 16]
        out.write("%06x  %-47s  %s\n"
                  % (a, " ".join("%02x" % c for c in row),
                     "".join(chr(c) if 32 <= c < 127 else "." for c in row)))


# -------------------------------------------------------------------- main

def pick_blob(args):
    """The bytes the subcommand works on, plus a Pef if one applies."""
    raw = open(args.file, "rb").read()
    data, rsrc = split_forks(raw)
    if args.rsrc:
        rtype, _, rid = args.rsrc.partition(":")
        blob = resource_blob(rsrc, rtype.ljust(4)[:4],
                             int(rid, 0) if rid else None)
        if blob is None:
            sys.exit("no %s resource" % args.rsrc)
    else:
        blob = rsrc if args.fork == "rsrc" else data
    pef = None
    if getattr(args, "container", None) is not None:
        pef = Pef(blob, args.container)
        if getattr(args, "section", "code") == "code":
            blob = pef.code()
        else:
            blob = pef.data()
    return blob, pef, data, rsrc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("info", "forks", "rsrc", "pef", "sections",
                                        "imports", "exports", "toc", "reloc",
                                        "dis", "data"))
    ap.add_argument("file")
    ap.add_argument("-c", "--container", type=lambda s: int(s, 0),
                    help="offset of the Joy!peff container")
    ap.add_argument("-a", "--at", type=lambda s: int(s, 0), default=0)
    ap.add_argument("-n", "--count", type=lambda s: int(s, 0), default=48)
    ap.add_argument("--arch", choices=("ppc", "68k"), default="ppc")
    ap.add_argument("--fork", choices=("data", "rsrc"), default="data")
    ap.add_argument("--rsrc", help="read TYPE[:ID] out of the resource fork")
    ap.add_argument("--section", choices=("code", "data"), default="code")
    ap.add_argument("--type", help="resource type filter")
    ap.add_argument("--id", type=lambda s: int(s, 0), help="resource id filter")
    ap.add_argument("--out", help="write the bytes out instead of listing")
    args = ap.parse_args()

    raw = open(args.file, "rb").read()
    data, rsrc = split_forks(raw)

    if args.command == "info":
        h = macbinary_header(raw)
        print("file      %s (%d bytes)" % (args.file, len(raw)))
        if h:
            print("macbinary %r type %r creator %r" % (h["name"], h["type"],
                                                       h["creator"]))
        else:
            print("macbinary no wrapper - treating the whole file as data")
        print("data fork %d bytes" % len(data))
        print("rsrc fork %d bytes" % len(rsrc))
        for label, blob in (("data", data), ("rsrc", rsrc)):
            for off in containers(blob):
                print("  PEF in %s fork at %#08x  %s"
                      % (label, off, " | ".join(names_near(blob, off)[-2:])))
        types = {}
        for rtype, _rid, _n, _o, _l in resources(rsrc):
            types[rtype] = types.get(rtype, 0) + 1
        if types:
            print("resources " + ", ".join("%s x%d" % (t, n)
                                           for t, n in sorted(types.items())))
        return

    if args.command == "forks":
        base = args.out or args.file.rsplit(".", 1)[0]
        open(base + ".data", "wb").write(data)
        open(base + ".rsrc", "wb").write(rsrc)
        print("wrote %s.data (%d) and %s.rsrc (%d)"
              % (base, len(data), base, len(rsrc)))
        return

    if args.command == "rsrc":
        for rtype, rid, name, off, rlen in resources(rsrc):
            if args.type and rtype != args.type.ljust(4)[:4]:
                continue
            if args.id is not None and rid != args.id:
                continue
            if args.out:
                open(args.out, "wb").write(rsrc[off:off + rlen])
                print("wrote %s (%d bytes) from %s %d" % (args.out, rlen, rtype, rid))
                return
            print("%-4s %6d  %7d bytes  %s" % (rtype, rid, rlen, name))
        return

    if args.command == "pef":
        for label, blob in (("data", data), ("rsrc", rsrc)):
            for off in containers(blob):
                p = Pef(blob, off)
                code = p.section(0)
                print("%s %#08x  code %#08x+%#x  toc %#x  %s"
                      % (label, off, code["coff"] if code else 0,
                         code["clen"] if code else 0, p.toc(),
                         " | ".join(names_near(blob, off)[-2:])))
        return

    blob, pef, data, rsrc = pick_blob(args)

    if args.command == "sections":
        for s in pef.sections:
            print("%d kind %d addr %08x total %06x unpacked %06x clen %06x "
                  "file %08x" % (s["index"], s["kind"], s["addr"], s["total"],
                                 s["unpacked"], s["clen"], s["coff"]))
    elif args.command == "imports":
        libs, syms = pef.imports()
        for name, first, cnt in libs:
            print("  %s (%d)" % (name, cnt))
            for i in range(first, first + cnt):
                print("    %3d %s" % (i, syms[i]))
    elif args.command == "exports":
        d = pef.data()
        for name, sect, value, cls in sorted(pef.exports()):
            if sect == 1 and value + 4 <= len(d):
                print("%-44s data+%06x -> code+%06x"
                      % (name, value, struct.unpack_from(">I", d, value)[0]))
            else:
                print("%-44s sect %d off %#x class %d" % (name, sect, value, cls))
    elif args.command == "toc":
        rel = pef.relocations()
        print("r2 = data+%#x" % pef.toc())
        for pos in sorted(rel):
            print("  data+%06x  (r2%+d)  %s" % (pos, pos - pef.toc(), rel[pos]))
    elif args.command == "reloc":
        rel = pef.relocations()
        for pos in sorted(rel):
            print("  data+%06x  %s" % (pos, rel[pos]))
    elif args.command == "dis":
        disassemble(blob, args.at, args.count, args.arch, pef)
    elif args.command == "data":
        hexdump(pef.data() if pef else blob, args.at, args.count)


if __name__ == "__main__":
    main()
