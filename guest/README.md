# Mac OS 9 guest tools

## Instructions 

Built with Retro68 (PowerPC). On the Linux VM:

```
TC=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake -S guest/sheepshears -B build-sheepshears -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC && ninja -C build-sheepshears
cmake -S guest/sheepbench -B build-sheepbench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC && ninja -C build-sheepbench
```

Copy `SheepShears.bin` / `SheepBench.bin` into the extfs folder with `emumonitor deploy`.

### SheepShears

Background-only tool that needs to be put in Startup Items on the guest.

### SheepBench

Just a standard guest app, you can put anywhere.

## Program Descriptions

### SheepShears 

Shuts down the OS gracefully through the finder without a dialog (no "Shut down?" dialog) when the SheepShaver window is closed. 30 tick polling rate. Without SheepShears the OS just hard powers off when the SheepShaver window is closed (the standard way).

### SheepBench 

Console-like benchmark that benches CPU, FPU, memory kernels, QuickDraw and disk throughput and does an FPU integrity check and a `MakeDataExecutable` code-patch check. Results print in the guest as well as going to the host's stdout and are appended to `SheepBench.txt` in the host's SheepShaver's working directory.


## Backend Host SheepShaver Implementation Details

Both talk to the host through `shearsclient.h`: SheepShaver writes `'SHRS'` at location `0x2860` and the native mailbox routine at `0x2864`. The tools never call it unless both that and `'Baah'` at `0x2800` are present.
