#!/usr/bin/env python3
"""Rewrite GCC 16 addr32 guest-memory ops to through-rax/rdx form
that stock patch_jit.pl keys recognize.

Invoked by regenerate_x86_64.sh on a work-dir copy of the extracted
headers, before patch_jit.pl / patch_jit_membulk.py.
"""
from pathlib import Path
import re
import sys

# Exact byte-sequence rewrites. Applied left-to-right, non-overlapping.
# Keep address computation in eax/edx so TRANS_RAX / TRANS_RDX can wrap it.
REPLACEMENTS = [
    # --- basic loads from T1 (r13) ---
    # addr32 mov r12d, [r13d+0]
    (bytes.fromhex("67 45 8b 65 00"), bytes.fromhex("44 89 e8 44 8b 00")),
    # addr32 mov r12d, [r13d+r14d+0]
    (bytes.fromhex("67 47 8b 64 35 00"), bytes.fromhex("43 8d 04 2e 44 8b 00")),
    # addr32 movzx eax, word [r13d+0]
    (bytes.fromhex("67 41 0f b7 45 00"), bytes.fromhex("44 89 e8 0f b7 00")),
    # addr32 movzx eax, word [r13d+r14d+0]
    (bytes.fromhex("67 43 0f b7 44 35 00"), bytes.fromhex("43 8d 04 2e 0f b7 00")),
    # addr32 movzx r12d, byte [r13d+0]
    (bytes.fromhex("67 45 0f b6 65 00"), bytes.fromhex("44 89 e8 44 0f b6 20")),
    # addr32 movzx r12d, byte [r13d+r14d+0]
    (bytes.fromhex("67 47 0f b6 64 35 00"), bytes.fromhex("43 8d 04 2e 44 0f b6 20")),
    # addr32 movsx r12d, byte [r13d+0]
    (bytes.fromhex("67 45 0f be 65 00"), bytes.fromhex("44 89 e8 44 0f be 20")),
    # addr32 movsx r12d, byte [r13d+r14d+0]
    (bytes.fromhex("67 47 0f be 64 35 00"), bytes.fromhex("43 8d 04 2e 44 0f be 20")),
    # --- basic stores to T1 (r13) ---
    # addr32 mov [r13d+0], eax
    (bytes.fromhex("67 41 89 45 00"), bytes.fromhex("44 89 ea 89 02")),
    # addr32 mov [r13d+r14d+0], eax
    (bytes.fromhex("67 43 89 44 35 00"), bytes.fromhex("43 8d 14 2e 89 02")),
    # addr32 mov [r13d+0], ax  (66 prefix before 67 in source; 67 then 66 rex)
    (bytes.fromhex("67 66 41 89 45 00"), bytes.fromhex("44 89 ea 66 89 02")),
    (bytes.fromhex("67 66 43 89 44 35 00"), bytes.fromhex("43 8d 14 2e 66 89 02")),
    # addr32 mov [r13d+0], r12b
    (bytes.fromhex("67 45 88 65 00"), bytes.fromhex("44 89 e8 44 88 20")),
    # addr32 mov [r13d+r14d+0], r12b
    (bytes.fromhex("67 47 88 64 35 00"), bytes.fromhex("43 8d 04 2e 44 88 20")),
    # --- ppc / lmw / stmw through T0 (r12) ---
    # addr32 mov eax, [r12d]
    (bytes.fromhex("67 41 8b 04 24"), bytes.fromhex("44 89 e0 8b 00")),
    # addr32 mov edx, [r12d]  -> through eax so 8b00 matches
    (bytes.fromhex("67 41 8b 14 24"), bytes.fromhex("44 89 e0 8b 00 89 c2")),
    # addr32 mov [r12d], eax
    (bytes.fromhex("67 41 89 04 24"), bytes.fromhex("44 89 e2 89 02")),
    # addr32 mov [r12d], edx
    (bytes.fromhex("67 41 89 14 24"), bytes.fromhex("44 89 e0 89 10")),
    # im-form mov r12d, [rdx+rax] -> mov eax,[rdx+rax]; mov r12d,eax (key 8b0402)
    (bytes.fromhex("44 8b 24 02"), bytes.fromhex("8b 04 02 41 89 c4")),
    # addr32 mov eax, [eax]  (after prior mov eax,r12d)
    (bytes.fromhex("67 8b 00"), bytes.fromhex("8b 00")),
    # addr32 mov eax, [edx]
    (bytes.fromhex("67 8b 02"), bytes.fromhex("8b 02")),
    # addr32 mov [r13d], edx  (stwcx)
    (bytes.fromhex("67 41 89 55 00"), bytes.fromhex("44 89 e8 89 10")),
    # addr32 mov [edx], eax
    (bytes.fromhex("67 89 02"), bytes.fromhex("89 02")),
    # addr32 mov eax, [ecx]  (lmw_im walking ptr) -> eax=ecx; [rax]
    (bytes.fromhex("67 8b 01"), bytes.fromhex("89 c8 8b 00")),
    # addr32 mov [ecx], eax  (stmw_im) -> keep value in eax, addr via rdx
    (bytes.fromhex("67 89 01"), bytes.fromhex("52 89 ca 89 02 5a")),
    # --- FP / vector through T1 (r13) as 64-bit addr32 ---
    # addr32 mov rax, [r13d]
    (bytes.fromhex("67 49 8b 45 00"), bytes.fromhex("44 89 e8 48 8b 00")),
    # addr32 mov rax, [r13d+r14d]
    (bytes.fromhex("67 4b 8b 44 35 00"), bytes.fromhex("43 8d 04 2e 48 8b 00")),
    # addr32 mov [r13d], rax
    (bytes.fromhex("67 49 89 45 00"), bytes.fromhex("44 89 ea 48 89 02")),
    # addr32 mov [r13d+r14d], rax
    (bytes.fromhex("67 4b 89 44 35 00"), bytes.fromhex("43 8d 14 2e 48 89 02")),
    # addr32 mov eax, [r13d]
    (bytes.fromhex("67 41 8b 45 00"), bytes.fromhex("44 89 e8 8b 00")),
    # addr32 mov eax, [r13d+r14d]
    (bytes.fromhex("67 43 8b 44 35 00"), bytes.fromhex("43 8d 04 2e 8b 00")),
    # --- dcbz: SSE zero -> stock four movq form (matches DCBZ_OLD) ---
    (bytes.fromhex("41 83 e4 e0 66 0f ef c0 44 89 e0 67 41 0f 29 04 24 0f 29 40 10"),
     bytes.fromhex("44 89 e0 83 e0 e0 41 89 c4 89 c0 "
                   "48 c7 00 00 00 00 00 "
                   "48 c7 40 08 00 00 00 00 "
                   "48 c7 40 10 00 00 00 00 "
                   "48 c7 40 18 00 00 00 00")),
]

# disp8 variants of [r12d+disp] / [edx+disp] / [eax+disp]
for disp in range(4, 32, 4):
    db = bytes([disp])
    REPLACEMENTS.extend([
        # addr32 mov edx, [r12d+disp] -> eax=r12d+disp; [rax]; edx=eax
        (bytes.fromhex("67 41 8b 54 24") + db,
         bytes.fromhex("44 89 e0 83 c0") + db + bytes.fromhex("8b 00 89 c2")),
        # addr32 mov eax, [r12d+disp]
        (bytes.fromhex("67 41 8b 44 24") + db,
         bytes.fromhex("44 89 e0 83 c0") + db + bytes.fromhex("8b 00")),
        # addr32 mov [r12d+disp], edx  (key 8910)
        (bytes.fromhex("67 41 89 54 24") + db,
         bytes.fromhex("44 89 e0 83 c0") + db + bytes.fromhex("89 10")),
        # addr32 mov [r12d+disp], eax  (value in eax: use rdx, key 8902)
        (bytes.fromhex("67 41 89 44 24") + db,
         bytes.fromhex("44 89 e2 83 c2") + db + bytes.fromhex("89 02")),
        # addr32 [edx+disp]: strip 67 so a prior TRANS_RDX on edx covers it
        (bytes.fromhex("67 8b 42") + db, bytes.fromhex("8b 42") + db),
        (bytes.fromhex("67 89 42") + db, bytes.fromhex("89 42") + db),
        # addr32 mov eax, [eax+disp] after lea r12, [r12+disp] => [r12]
        (bytes.fromhex("67 8b 40") + db,
         bytes.fromhex("44 89 e0 8b 00")),
        # addr32 mov [eax+disp], edx after lea r12, [r12+disp] => [r12]
        (bytes.fromhex("67 89 50") + db,
         bytes.fromhex("44 89 e0 89 10")),
        # lmw/stmw: lea r12, [r12+disp]; [edx+disp] uses original T0 copy
        (bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("8b 42") + db,
         bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("44 89 e0 8b 00")),
        (bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("67 8b 42") + db,
         bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("44 89 e0 8b 00")),
        (bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("89 42") + db,
         bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("44 89 e0 89 10")),
        (bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("67 89 42") + db,
         bytes.fromhex("45 8d 64 24") + db + bytes.fromhex("44 89 e0 89 10")),
    ])

REPLACEMENTS.sort(key=lambda p: -len(p[0]))

HEX_RE = re.compile(r"0x([0-9a-fA-F]{2})")
COPY_RE = re.compile(r"(copy_block\([A-Za-z0-9_]+, )(\d+)(\);)")
INC_RE = re.compile(r"(inc_code_ptr\()(\d+)(\);)")
PTR_RE = re.compile(r"(code_ptr\(\) \+ )(\d+)")
ARRAY_RE = re.compile(
    r"(static const uint8 [A-Za-z0-9_]+_code\[\] = \{)(.*?)(\n    \};)",
    re.S)
GEN_RE = re.compile(r"DEFINE_GEN\(gen_op_[A-Za-z0-9_]+.*?#endif\n", re.S)


def apply_replacements(buf: bytes):
    """Return (new_bytes, list of (old_index, delta))."""
    out = bytearray()
    shifts = []
    i = 0
    n = len(buf)
    while i < n:
        matched = False
        for old, new in REPLACEMENTS:
            if buf.startswith(old, i):
                shifts.append((len(out), len(new) - len(old)))
                out += new
                i += len(old)
                matched = True
                break
        if not matched:
            out.append(buf[i])
            i += 1
    return bytes(out), shifts


def emit_array(buf: bytes) -> str:
    parts = []
    for i, b in enumerate(buf):
        if i % 12 == 0:
            parts.append("\n       ")
        else:
            parts.append(" ")
        parts.append("0x%02x" % b)
        if i + 1 < len(buf):
            parts.append(",")
    return "".join(parts)


def remap_offset(off: int, shifts):
    delta = 0
    for pos, d in shifts:
        # shifts recorded in NEW buffer coordinates at insertion start.
        # Easier: compute from original: we recorded (new_pos, delta) at
        # replacement start in the NEW stream, which equals old pos + prior
        # deltas. Use a simpler pass: apply sequentially on original index.
        pass
    return off


def rewrite_block(block: str) -> str:
    m = ARRAY_RE.search(block)
    if not m:
        return block
    raw = bytes(int(h, 16) for h in HEX_RE.findall(m.group(2)))
    if not raw:
        return block
    new, shifts_new = apply_replacements(raw)
    if new == raw:
        return block

    # Map original offsets -> new offsets. Replay replacements on original.
    orig_to_new = list(range(len(raw) + 1))
    i = 0
    ni = 0
    orig_index_at_new = []
    while i < len(raw):
        matched = False
        for old, newb in REPLACEMENTS:
            if raw.startswith(old, i):
                d = len(newb) - len(old)
                # everything after old in original shifts by d
                for k in range(i + len(old), len(orig_to_new)):
                    orig_to_new[k] += d
                i += len(old)
                ni += len(newb)
                matched = True
                break
        if not matched:
            i += 1
            ni += 1

    def map_off(off):
        if off < 0 or off > len(raw):
            return off
        return orig_to_new[off]

    body = emit_array(new)
    block = block[:m.start(2)] + body + block[m.end(2):]
    block = COPY_RE.sub(lambda mm: mm.group(1) + str(len(new)) + mm.group(3), block, count=1)
    block = INC_RE.sub(lambda mm: mm.group(1) + str(len(new)) + mm.group(3), block, count=1)

    def ptr_sub(mm):
        return mm.group(1) + str(map_off(int(mm.group(2))))
    block = PTR_RE.sub(ptr_sub, block)
    return block


def rewrite_file(path: Path):
    text = path.read_text(encoding="latin-1")
    n = 0
    def sub(m):
        nonlocal n
        nb = rewrite_block(m.group(0))
        if nb != m.group(0):
            n += 1
        return nb
    new = GEN_RE.sub(sub, text)
    path.write_text(new, encoding="latin-1")
    leftover_ops = []
    for m in GEN_RE.finditer(new):
        if "0x67" in m.group(0):
            nm = re.search(r"DEFINE_GEN\((gen_op_[A-Za-z0-9_]+)", m.group(0))
            leftover_ops.append(nm.group(1) if nm else "?")
    leftover = len(re.findall(r"0x67", new))
    print(f"{path.name}: rewrote {n} ops, remaining 0x67 count={leftover}")
    if leftover_ops:
        print("  leftover:", ", ".join(leftover_ops[:30]))


def main():
    hdr = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    for name in ("basic-dyngen-ops-x86_64.hpp", "ppc-dyngen-ops-x86_64.hpp"):
        p = hdr / name
        if not p.exists():
            sys.exit(f"missing {p}")
        rewrite_file(p)


if __name__ == "__main__":
    main()
