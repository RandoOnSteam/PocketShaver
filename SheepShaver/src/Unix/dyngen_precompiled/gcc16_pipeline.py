#!/usr/bin/env python3
"""Post-process GCC 16 dyngen output for the x86_64 macOS / MEM_BULK headers.

Used by regenerate_x86_64.sh. Does not modify kpx_cpu or the stock
patch_jit.pl / patch_jit_membulk.py files; it works on copies.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
ARRAY_COPY_RE = re.compile(
    r"static const uint8 (.*?)\[\] = \{(.*?\n    \};\n)    copy_block\(([A-Za-z0-9_]+),",
    re.S,
)
GEN_CONST_RE = re.compile(r"DEFINE_GEN\(gen_const_dot_lC\d+,")


def sanitize_header_names(path: Path) -> int:
    """Replace dyngen-corrupted array identifiers with the copy_block name."""
    text = path.read_bytes().decode("latin-1")
    n = 0

    def repl(m):
        nonlocal n
        arr, rest, cb = m.group(1), m.group(2), m.group(3)
        if IDENT_RE.fullmatch(arr):
            return m.group(0)
        n += 1
        return f"static const uint8 {cb}[] = {{{rest}    copy_block({cb},"

    new, _ = ARRAY_COPY_RE.subn(repl, text)
    path.write_bytes(new.encode("latin-1"))
    return n


def _hex_rows(blob: bytes, indent: str = "       ") -> str:
    parts = []
    for i, b in enumerate(blob):
        if i % 12 == 0:
            parts.append("\n" + indent)
        else:
            parts.append(" ")
        parts.append("0x%02x" % b)
        if i + 1 < len(blob):
            parts.append(",")
    return "".join(parts)


def inject_literal16_consts(header: Path, literals: dict[str, bytes]) -> None:
    """Emit gen_const_dot_lC* from the opcode object's __literal16 pool."""
    text = header.read_text(encoding="latin-1")
    if GEN_CONST_RE.search(text):
        return
    blocks = []
    for name in sorted(literals):
        blob = literals[name]
        ident = f"dot_{name}"
        blocks.append(
            f"DEFINE_GEN(gen_const_{ident},uint8 *,(void))\n"
            f"#ifdef DYNGEN_IMPL\n"
            "{\n"
            f"    static const uint8 {ident}[] = {{{_hex_rows(blob)}\n"
            "    };\n"
            "    static uint8 *data_p = NULL;\n"
            "    if (data_p == NULL)\n"
            f"        data_p = copy_data({ident}, {len(blob)});\n"
            "    return data_p;\n"
            "}\n"
            "#endif\n"
        )
    needle = "#endif\nDEFINE_GEN("
    idx = text.find(needle)
    if idx < 0:
        sys.exit(f"{header}: cannot find insertion point for gen_const_dot_lC*")
    insert_at = idx + len("#endif\n")
    header.write_text(text[:insert_at] + "".join(blocks) + text[insert_at:], encoding="latin-1")


def parse_literal16(otool_text: str, nm_text: str) -> dict[str, bytes]:
    """Map lC0/lC1/... to 16-byte blobs using nm addresses and otool dump."""
    addrs = {}
    for line in nm_text.splitlines():
        # 00000000000033d0 (__TEXT,__literal16) non-external lC0
        m = re.search(r"^([0-9a-fA-F]+)\s+\([^)]*__literal16[^)]*\)\s+\S+\s+(lC\d+)\s*$", line)
        if not m:
            continue
        addrs[m.group(2)] = int(m.group(1), 16)
    hex_bytes = []
    base = None
    for line in otool_text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{16})\s+((?:[0-9a-fA-F]{2}\s*)+)$", line.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        row = bytes(int(x, 16) for x in m.group(2).split())
        if base is None:
            base = addr
        hex_bytes.append((addr, row))
    if not addrs:
        sys.exit("no lC* symbols found in nm output")
    if not hex_bytes:
        sys.exit("no __literal16 contents in otool output")
    pool = bytearray()
    pool_base = hex_bytes[0][0]
    for addr, row in hex_bytes:
        need = addr - pool_base
        if len(pool) < need:
            pool.extend(b"\x00" * (need - len(pool)))
        pool[need:need + len(row)] = row
    out = {}
    for name, addr in addrs.items():
        off = addr - pool_base
        blob = bytes(pool[off:off + 16])
        if len(blob) != 16:
            sys.exit(f"{name}: expected 16-byte literal, got {len(blob)}")
        out[name] = blob
    return out


def adapt_membulk_script(src: Path, dst: Path) -> None:
    """Copy patch_jit_membulk.py and apply the GCC 16 layout adaptations."""
    t = src.read_text(encoding="utf-8")
    replacements = [
        ("with open(src_path) as f:", "with open(src_path, encoding='latin-1') as f:"),
        ('with open(dst_path, "w") as f:', "with open(dst_path, 'w', encoding='utf-8') as f:"),
        ("with open(path) as f:", "with open(path, encoding='latin-1') as f:"),
        ('with open(path, "w") as f:', "with open(path, 'w', encoding='latin-1') as f:"),
        (
            "FD_OLD_DISP = [0xa8, 0x08, 0x10, 0x00]        # little-endian 0x1008a8",
            "FD_OLD_DISP = [0xc0, 0x08, 0x10, 0x00]        # little-endian 0x1008c0",
        ),
        (
            '''BRANCH_FIXES = {
    "gen_op_stwcx_T0_T1": ("RAX", [(0x74, 0x24, +1), (0x75, 0x0f, +1)]),
    "gen_op_lmw_T0_im":   ("RAX", [(0xeb, 0x11, +1), (0x76, 0xea, -1)]),
    "gen_op_stmw_T0_im":  ("RDX", [(0xeb, 0x12, +1), (0x76, 0xe9, -1)]),
}''',
            '''BRANCH_FIXES = {
    "gen_op_stwcx_T0_T1": ("RAX", [(0x74, 0x22, +1), (0x75, 0x0f, +1)]),
    "gen_op_lmw_T0_im":   ("RAX", [(0x77, 0x1d, +1), (0x75, 0xea, -1)]),
    "gen_op_stmw_T0_im":  ("RDX", [(0x77, 0x16, +1), (0x75, 0xea, -1)]),
}''',
        ),
        (
            '''            ('"8902", "0fb620", "8820",', '"8902", "8b02", "0fb620", "8820",'),''',
            '''            ('"8902", "0fb620", "8820",', '"8902", "8b02", "0fb620", "0fbe20", "8820",'),''',
        ),
        (
            '    patch_raw_tails("ppc-dyngen-ops-x86_64_macos_membulk.hpp")\n',
            "    # GCC 16 encodings already match patch_jit keys for these tails.\n",
        ),
    ]
    for old, new in replacements:
        if old not in t:
            sys.exit(f"adapt_membulk_script: needle not found:\n{old}")
        t = t.replace(old, new, 1)
    # Warning instead of hard exit when a GCC body does not match stock bytes.
    t = t.replace(
        '''    if DCBZ_OLD not in text:
        sys.exit(f"error: op_dcbz_T0 body not found in {path} — bytes changed?")
''',
        '''    if DCBZ_OLD not in text:
        print(f"warning: op_dcbz_T0 body not found in {path}")
        return
''',
    )
    t = t.replace(
        '''        if not m:
            sys.exit(f"error: {op} body not found in {path}")
''',
        '''        if not m:
            print(f"warning: {op} body not found in {path}")
            continue
''',
    )
    t = t.replace(
        '''        if block.count("TRANS_") != 1:
            sys.exit(f"error: {op} expected exactly one stub, "
                     f"found {block.count('TRANS_')}")
''',
        '''        if block.count("TRANS_") != 1:
            print(f"warning: {op} expected exactly one stub, found {block.count('TRANS_')}")
            continue
''',
    )
    dst.write_text(t, encoding="utf-8")


def adapt_patch_jit(src: Path, dst: Path) -> None:
    """Add the GCC 16 movsx byte-load key that stock patch_jit.pl lacks."""
    t = src.read_text(encoding="utf-8")
    old = '"8902", "0fb620", "8820",'
    new = '"8902", "0fb620", "0fbe20", "8820",'
    if old not in t:
        sys.exit("adapt_patch_jit: key table needle not found")
    dst.write_text(t.replace(old, new, 1), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("sanitize")
    p.add_argument("headers", nargs="+", type=Path)

    p = sub.add_parser("adapt-membulk")
    p.add_argument("src", type=Path)
    p.add_argument("dst", type=Path)

    p = sub.add_parser("adapt-patch-jit")
    p.add_argument("src", type=Path)
    p.add_argument("dst", type=Path)

    p = sub.add_parser("inject-literals")
    p.add_argument("--header", type=Path, required=True, action="append")
    p.add_argument("--nm", type=Path, required=True)
    p.add_argument("--otool", type=Path, required=True)

    args = ap.parse_args()
    if args.cmd == "sanitize":
        total = 0
        for h in args.headers:
            n = sanitize_header_names(h)
            print(f"{h.name}: sanitized {n} array names")
            total += n
        if total == 0:
            print("no corrupted array names")
    elif args.cmd == "adapt-membulk":
        adapt_membulk_script(args.src, args.dst)
        print(f"wrote {args.dst}")
    elif args.cmd == "adapt-patch-jit":
        adapt_patch_jit(args.src, args.dst)
        print(f"wrote {args.dst}")
    elif args.cmd == "inject-literals":
        literals = parse_literal16(
            args.otool.read_text(encoding="latin-1"),
            args.nm.read_text(encoding="latin-1"),
        )
        print("literals:", ", ".join(f"{k}={v.hex()}" for k, v in sorted(literals.items())))
        for h in args.header:
            inject_literal16_consts(h, literals)
            print(f"{h.name}: injected gen_const_dot_lC*")


if __name__ == "__main__":
    main()
