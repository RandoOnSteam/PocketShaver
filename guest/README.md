# Mac OS 9 guest tools

## Instructions 

Built with Retro68 (PowerPC). Retro68 commandline:

```
TC=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake -S guest/sheepshears -B build-sheepshears -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC && ninja -C build-sheepshears
cmake -S guest/sheepbench -B build-sheepbench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC && ninja -C build-sheepbench
```

The SheepBench build also needs the build machine's `cc` - it runs `sbexpected.c` to compute the kernel checksums into `sbexpected.h`. It also needs Retro68's `m68k-apple-macos-as`/`objcopy` (the 68k kernels) and `xxd`.

Copy `SheepShears.bin`, `SheepShearsInstaller.bin`, `SheepBench.bin` and `SheepBenchCurses.bin` into the extfs folder with `emumonitor deploy`.

### SheepShears

Background-only tool that needs to be put in Startup Items on the guest. `SheepShearsInstaller` does that: put it next to `SheepShears`, open it and click Install (it quits a running copy first, copies the tool, then starts it). Remove takes it out again. A file named `Sheep Shears Auto Install` in the System Folder installs it unattended (without a GUI).

### SheepBench

Just a standard guest app, you can put anywhere. It asks which suites to run, whether to do a quick run, and (when the host allows it) whether to repeat everything with the JIT off. The same build also makes `SheepBenchCurses`, the same benchmark with a windowed text UI through PDCursesMod's Mac port (the `guest/pdcurses` submodule; run `git submodule update --init` first). It is the same source compiled with `SHEEPBENCHCURSES` (`benchui.c` holds both front ends). 

## Program Descriptions

### SheepShears 

Shuts down the OS gracefully through the finder without a dialog (no "Shut down?" dialog) when the SheepShaver window is closed. 30 tick polling rate. Without SheepShears the OS just hard powers off when the SheepShaver window is closed (the standard way).

### SheepBench 

Console benchmark with seven suites:

| Suite | Measures | Checked by |
|---|---|---|
| `cpu` | Seven hand-written instruction loops (byte copy, the ROM BlockMove loop, a MacBench-style compare loop, calls in the same page and another page, `bctrl`, conditional `blr`) in ns per instruction; the shared C kernels (integer, byte copy, compare, direct and indirect calls, `lwarx`/`stwcx.`, double and single multiply-add, a vector loop in C and the same loop in AltiVec); the FPU integrity runs | the copies, loop and call counts, each kernel's build-time checksum and the host's native checksum |
| `68k` | add/eor/rol, `move.l` and `move.b` copies, `bsr`/`rts`, `mulu`/`divu`, run by the ROM's 68k emulator through Mixed Mode | the results worked out in C, the copies |
| `memory` | memcpy, an 8-byte copy loop, BlockMove, BlockMoveData, memset and dependent-load latency at 4 KB, 64 KB, 1 MB, 16 MB | checksums of the destination |
| `mmu` | page size and VM, loads one per page over 16/256/4096 pages against loads in one page, first write to each page of a new 4 MB block, rewrite + MakeDataExecutable + call (self-modifying code), MakeDataExecutable alone | load sums, the rewritten routine's return value |
| `quickdraw` | 13 operations (fills, invert, pattern, frames, lines, CopyBits from 32/16/8-bit, srcXor, srcOr, ScrollRect, text, icons) at 64 x 64, 512 x 512 and full screen | the same drawing by QuickDraw in an offscreen GWorld, compared pixel by pixel where the window is visible |
| `3d` | every RAVE engine that accepts a 640 x 480 ARGB32 offscreen context (flat, Gouraud, textured, Z + blend, Z + fog at 100/1000/10000 triangles) and a QuickDraw 3D scene with the default engine and with PocketShaver's engine asked for | a red triangle on blue per engine; a triangle over the middle of the QuickDraw 3D picture |
| `disk` | a 32 MB file on each writable volume (at most three), sequential and random reads and writes at 512 B, 4 KB, 64 KB, 1 MB, no-cache flag; each row names the image type from the host (raw, qcow2, chd, extfs, ...) | every read against what was written |

Each repetition is made at least 20 ms long, at least five are timed and the median is used; repetitions are added until the median absolute deviation is under 2 % of the median, or 25 repetitions or the test's 3 second budget (quick runs: 10 ms, 9 repetitions, 1 second). A result that never settles is marked `noisy`. Command-period stops a run between repetitions.

The C kernels in `sbkernels.c` are compiled into SheepShaver as well; the host runs the same kernel natively and the row shows the native rate and the percentage of native speed.

With the pref `benchswitch true` the host lets SheepBench switch the PowerPC JIT off and back on (only a JIT that was on at start-up). The whole run is then repeated with the JIT off and a table of JIT on / JIT off / speedup ends the output. The switch is applied between blocks on the CPU thread (`SPCFLAG_JIT_SWITCH`), never through a prefs write.

Results print in the guest, go to the host's stdout and are appended to `SheepBench.txt` in SheepShaver's working directory. A tab-separated `SheepBench Report` is written next to the app.

Auto mode: a file `SheepBench Auto` next to the app or in the System Folder holds lines such as `suites cpu memory`, `quick 1`, `jit 1` (also run with the JIT off) and `shutdown 1` (ask the Finder to shut down when done). SheepBench then runs without asking and quits at the end.


## Backend Host SheepShaver Implementation Details

Both talk to the host through `shearsclient.h`: SheepShaver writes `'SHRS'` at location `0x2860` and the native mailbox routine at `0x2864`. The tools never call it unless both that and `'Baah'` at `0x2800` are present.

| Command | Number | What it does |
|---|---|---|
| hello | 1 | the tool connects; the reply carries the poll interval |
| poll | 2 | the tool's heartbeat; the reply carries the shutdown request flag |
| log | 3 | a line for the host's stdout |
| result | 5 | the tool's answer to a shutdown request |
| info | 7 | `key=value` lines: PVR, time base, RAM, JIT state and how it started, `benchswitch`, host OS, one line per disk (`disk drive=N kind=raw/qcow2/chd/... ro=0/1`) and the shared folder |
| native | 8 | runs a `sbkernels.c` kernel natively: checksum and time per iteration (median of up to five timed runs of at least 2 ms) |
| setjit | 9 | JIT off (0) or back on (1); refused unless `benchswitch true` |
| report | 10 | a SheepBench result line, appended to `SheepBench.txt` |

`SheepBenchCurses` also has the following features:

- a menu to tick the suites, quick mode and the JIT-off pass, then Run;
- a run view with the live result log, the current test and a progress bar (Esc or Command-period stops between repetitions);
- View results: a scrollable table with value, noise, % of native, the JIT-off value and the speedup;
- Host information: what the host reported (JIT state, switching, disks and their image types);
- Save report: the tab-separated report under a name you pick;
- Compare: pick a saved report and see it next to the current results with the change in percent.

Keys: Up/Down and Space/Return in the menu, or R run, V results, H host, S save, C compare, Q quit; mouse clicks select menu lines.
