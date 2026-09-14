# `tools/macpef.py` - MacBinary, resource fork and PEF

One tool for what the joystick and USB work keeps needing: open a classic Mac
file, get at its forks, find the PEF fragments and 68k resources inside, and
disassemble them with imports resolved to names.

Needs `capstone` for the `dis` subcommand; everything else is pure Python.

## 1. Subcommands

| | |
|---|---|
| `info FILE` | MacBinary header, fork sizes, every PEF, resource census |
| `forks FILE [--out BASE]` | write `BASE.data` and `BASE.rsrc` |
| `rsrc FILE [--type T] [--id N] [--out F]` | list or extract resources |
| `pef FILE` | every `Joy!peff` in both forks, with its code section and TOC |
| `sections -c OFF` | PEF section table |
| `imports -c OFF` | imported libraries and symbols, in index order |
| `exports -c OFF` | exports, TVectors resolved to code offsets |
| `toc -c OFF` | where r2 points, and what every relocated data word holds |
| `reloc -c OFF` | the relocation result on its own |
| `dis -c OFF -a ADDR [-n N] [--arch ppc\|68k]` | disassemble |
| `data -c OFF -a ADDR -n N` | hex dump of the expanded data section |

Source selection, on any subcommand:

* `--fork data` (default) or `--fork rsrc`
* `--rsrc TYPE[:ID]` reads one resource and works on that instead - this is how
  you disassemble a 68k `INIT`, `ADBS` or `DRVR`
* `-c/--container OFF` picks a PEF; `--section data` disassembles or dumps the
  data section rather than the code section

## 2. What it knows about the formats

### MacBinary

Header at 0, data fork at 128, resource fork on the next 128-byte boundary;
lengths are big-endian longs at +83 and +87. A file with no wrapper is treated
as one big data fork, so the same commands work on a raw `.rsrc` or a fork
that has already been split out.

### Resource fork

Standard header (`dataOff`, `mapOff`, `dataLen`, `mapLen`), type list at
`map+24`, name list at `map+26`, 8-byte type entries, 12-byte reference
entries, and each resource preceded by its own length long. Names are Pascal
strings in the name list.

### PEF

Container magic `Joy!peff`, section table at +40, 28 bytes per section.

| kind | what |
|--:|---|
| 0 | code |
| 1 | unpacked data |
| 2 | **pattern-initialised** data - has to be expanded before any offset in it means anything |
| 4 | loader (imports, exports, relocations) |

`data()` expands kind 2 with the five pidata opcodes (Zero, blockCopy,
repeatedBlock, and the two interleave forms) and pads to the section's total
size, so a data offset from the code lands where you expect.

**Exported names are not NUL terminated.** The length lives in the top half of
the matching word in the export key table; getting this wrong silently
concatenates every export into one string.

**Relocations** are a full opcode stream, not a table:
`RelocBySectDWithSkip`, the `0x4` run group (sectC / sectD / tvector12 /
tvector8 / vtable8 / **import run**), the `0x5` small group (`SmByImport`,
set-sectC, set-sectD, section), the `0x6` group (`IncrPosition`, `SmRepeat`,
which re-runs the previous *n* opcodes), and the `0x7` large group
(`LgByImport`, `SetPosition`). `SmRepeat` recurses. Miss the import-run
sub-opcode and every glue stub gets named after the wrong thing.

## 3. The TOC problem

**PEF does not record where r2 points.** Without it, no r2-relative load can be
read, which is most of the interesting code.

The tool infers it. Every import has a glue stub of the form

```
lwz  r12, d(r2)
stw  r2, 20(r1)
lwz  r0, 0(r12)
lwz  r2, 4(r12)
mtctr r0
bctr
```

and the relocation stream always places import[0] at data offset 0, so
`r2 = data + -min(d)` over all such stubs. Different fragments land in
different places and both signs occur:

| fragment | r2 |
|---|--:|
| `InputSprocket Extension` container 0 (the ISp core) | `data+0x504` |
| `InputSprocket Extension` container `0x4fe40` (ISp CH) | `data+0x1bc` |
| `InputSprocket Joy` | `data+0` |
| `Descent II` | `data+0x8000` |

Check it with `toc`: the first line is the base, and the listing that follows
should start `data+000000 (r2-N) import[0] <something plausible>`.

## 4. Disassembly annotations

PowerPC output resolves two things that otherwise cost an hour each:

* `bl 0x7058` → `; -> import[53] TickCount`, by recognising the target as an
  import glue stub;
* `lwz r12, -0xe8(r2)` → `; import[53] TickCount`, or, when the word is not a
  relocated import, `; data+0x41c = 0000cafe` so tables can be followed.

68k output (`--arch 68k`) has no annotation - it is there for `INIT`, `ADBS`,
`DRVR` and `CODE` resources. Anything capstone will not decode is printed as
`.long` / `.word` and stepped over rather than truncating the listing, which
matters because these fragments are full of jump tables and F-line opcodes the
ROM's own 68k emulator uses.

## 5. Worked examples

```sh
# What is in here at all
python tools/macpef.py info "InputSprocket Extension.bin"

# The ISp CH JoyManager client: its poll, with imports named
python tools/macpef.py dis "InputSprocket Extension.bin" -c 0x4fe40 -a 0x361c -n 80

# Its d-pad jump table (9 entries, rose order)
python tools/macpef.py data "InputSprocket Extension.bin" -c 0x4fe40 -a 0x4d0 -n 48

# The ISp core's entry points, TVectors already resolved
python tools/macpef.py exports "InputSprocket Extension.bin" -c 0 | sort

# Descent II: the type 4 JoyManager sampler
python tools/macpef.py forks "Descent II.bin" --out /tmp/d2
python tools/macpef.py dis "Descent II.bin" -c 0 -a 0x10e90 -n 40

# A 68k control panel out of its resource fork
python tools/macpef.py rsrc "Firebird INIT.bin"
python tools/macpef.py dis "Firebird INIT.bin" --rsrc ADBS -a 0 -n 40 --arch 68k
```

## 6. What it does not do, and what still does it

* **The Mac OS ROM.** It is not a MacBinary file and its fragments are
  LZSS/parcel compressed. `tools/rom_decode.py` produces the flat 4 MB image
  first; `tools/rom_pef.py` enumerates the PEFs in that image. Note the runtime
  ROM *area* is 5 MB (`ROM_AREA_SIZE`), so anything at `ROMBase + 0x400000` or
  above - the relocated 68k emulator, for one - is not in the file at all and
  has to be dumped from guest memory.
* **Following a TVector to its code.** `exports` does it for exports because
  they are all in the data section; a TVector reached some other way has to be
  read with `data` (first word is the code offset, second the TOC).
* **Symbolising anything but imports.** Local routines stay numbers.

Older single-purpose scripts it replaces: `tools/isp_ext.py` (fork split),
`tools/isp_ch.py` (PEF + TOC + disassembly), `tools/pef_unpack.py`,
`tools/pef_reloc.py`, and the ad-hoc `tools/descent_rsrc*.py` resource readers.
`tools/ohci_uim_disasm.py` stays because it also knows how to find the OHCI
driver by its PCI match string.

## 7. Gotchas paid for once already

* An offset into a kind 2 data section means nothing until the section is
  expanded. `data` expands it; raw file offsets do not line up.
* A guest pc is not a file offset. Subtract the fragment's load address, and
  for the ROM subtract `ROMBase` (`0x40800000` in SheepShaver).
* In the ROM's 68k emulator, r24 is the 68k pc and r25 its interrupt level -
  but r24 is a *cached* value and is routinely mid-instruction, so it names the
  routine and not the instruction. `docs/codewarrior-ppc-debug-freeze.md` has
  the case where that cost real time.
* `0xfc12` / `0xfc14` in ROM 68k code are the ROM emulator's own call-native
  opcodes, not SheepShaver EMUL_OPs (those are `0xfe43 + selector`).
