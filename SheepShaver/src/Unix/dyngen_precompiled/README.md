# Precompiled dyngen opcode headers

SheepShaver's PowerPC JIT (`ENABLE_DYNGEN`) copies host machine-code
templates out of specially compiled opcode objects. Those templates live
here as generated headers:

| File | Role |
| --- | --- |
| `*-dyngen-ops-x86_64.hpp` | Raw dyngen extractor output |
| `*-dyngen-ops-x86_64_macos.hpp` | After `patch_jit.pl` (real-addressing TRANS stubs) |
| `*-dyngen-ops-x86_64_macos_membulk.hpp` | After `patch_jit_membulk.py` (Catalyst `MEM_BULK` translation) |

Do not hand-edit the generated headers. Regenerate them.

## Why GCC 16

Apple Clang rejects dyngen's fixed global registers (`r12`–`r15`) and
computed goto, so the opcode objects must be compiled with GNU C++.
Homebrew `gcc-16` / `g++-16` is the toolchain that currently produces a
working `gen_op_spcflags_set` (`lea` + `lock or` of the atomic mask at
`rbp+0x3b0`). Older checked-in x86_64 headers still had an `xchg` spinlock
against a layout that no longer exists; that hung the guest.

The host `dyngen` extractor is also built with gcc-16. Historically a
GCC-built extractor could heap-corrupt; the copy used here skips unknown
Mach-O load commands instead of aborting, which is what made extraction
succeed on a current macOS SDK.

## Regenerating x86_64 headers

From the repo root, or from this directory:

```
./regenerate_x86_64.sh
```

`--no-install` leaves the new headers in the work directory (printed at
the end) without copying them here.

Requirements:

- Homebrew GCC 16 (`gcc-16`, `g++-16`)
- `perl`, `python3`, `nm`, `otool`

`kpx_cpu` is not modified. The script copies `dyngen.c` and `vm.hpp` into
a temp dir and patches those copies:

1. Stub `mach-o/ppc/reloc.h` (removed from modern SDKs).
2. Strip the Apple/`MEM_BULK` kernel-window branch from `vm.hpp` so the
   opcode objects emit a bare guest-address dereference for `patch_jit.pl`
   to wrap.
3. In `dyngen.c`: skip unknown Mach-O load commands (`LC_BUILD_VERSION`);
   treat GCC's `lC*` symbols like `.LC*`; allow `gen_dot_prefix` without a
   leading `.`.
4. Compile `basic-dyngen-ops.cpp` / `ppc-dyngen-ops.cpp` with `g++-16`
   (`-O2 -fomit-frame-pointer -fno-pic` and the other dyngen opcode flags).
5. Run the gcc-16 `dyngen` extractor.
6. Sanitize array names (`gcc16_pipeline.py sanitize`) — dyngen sometimes
   writes garbage identifiers; the `copy_block` name is authoritative.
7. Rewrite GCC 16 `addr32` (`0x67`) memory ops into the through-`rax`/`rdx`
   forms `patch_jit.pl` keys recognize (`normalize_gcc16.py`).
8. Run copies of `patch_jit.pl` and `patch_jit_membulk.py` adapted for this
   GCC layout (`0fbe20` key, `FD_OLD_DISP` `0x1008c0`, updated
   `BRANCH_FIXES`, skip `patch_raw_tails` because the GCC encodings already
   match the keys, latin-1 I/O).
9. Inject `gen_const_dot_lC0/1/2` from the opcode object's `__literal16`
   section. GCC 16 emits `lC*` rather than `.LC*`, and Mach-O dyngen does
   not emit the `DEFINE_GEN` bodies on its own.

Expected counts from the last successful run:

- basic: 133 `DEFINE_GEN`; macos TRANS 25; membulk TRANS 27 (stock was 26)
- ppc: 869 `DEFINE_GEN`; macos TRANS 17; membulk TRANS 63 (stock was 72)
- `gen_op_spcflags_set` bytes start `48 8d 05 ... f0 09 85 b0 03 00 00`

The membulk TRANS count is lower than the previous Clang-era headers
because GCC 16's instruction selection does not always hit the same
`patch_jit.pl` keys. Do not close that gap by editing the generated
headers; extend `normalize_gcc16.py` or the membulk adapter and
regenerate.

## Files in this directory that are sources, not products

- `regenerate_x86_64.sh` — orchestrates the pipeline above
- `gcc16_pipeline.py` — name sanitizer, literal inject, adapter copies
- `normalize_gcc16.py` — GCC 16 `addr32` → through-`rax`/`rdx`
- `patch_jit.pl` — stock real-addressing rewrite (copied, not edited)
- `patch_jit_membulk.py` — stock `MEM_BULK` rewrite (copied and adapted
  at regen time; this file stays the Clang-era original)

## Other architectures

`*-arm64.hpp` and `*-x86_32.hpp` are produced by their own host
toolchains and are not covered by `regenerate_x86_64.sh`.
