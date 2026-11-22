# RandoOnSteam's MacEmu Fork

A fork of MacEmu - Basilisk II and SheepShaver - with a large amount of new features, bug fixes, performance improvements, and crash protections.

## Originally PocketShaver Derived

Originally derived from PocketShaver, this fork supports that build's boot UI when building for IPhone. The metal portion of PocketShaver is unable to be tested by RandoOnSteam so it might be various levels of stable.

PocketShaver extends the SheepShaver PowerPC emulation core with four Metal-accelerated graphics engines, a customizable on-screen gamepad, Bonjour peer-to-peer networking, a modern preferences system that adapts to the running platform, and (on Mac Catalyst) a PowerPC-to-arm64 JIT compiler. The upstream BasiliskII (68k) and desktop SheepShaver targets are preserved alongside the iOS-specific additions.

## Features

### New, Improved Classic JIT

MacEmu now has an optional new JIT, called the "Native JIT", that is 5-10% faster on average and much faster in stuffit decompression. It can also be fully compiled with Microsoft Visual C++, in addition to GCC/Clang and more.

### GPU Acceleration

MacEmu now renders using native low and high level GPU Acceleration APIs through the gfxaccel option:

* **SDL1, SDL2, and SDL3 with OpenGL**- Everything is redirected and rendered with OpenGL with some light shader support
* **SDLGPU** - Renders with various APIs like DirectX 12 or Metal
* **Metal** - PocketShaver

#### GPU accelerated guest APIs supported

* **QuickDraw 3D RAVE** - Requires the extensions to be installed

* **3DFX Glide 2 and 3** - Requires the extensions - 3dfx GlideLib2.x and 3dfx GlideLib3.x - to be installed. Note that normal installation may not work and you may need to manually install by dragging all the extensions from the driver installation disk to your System folder.

* **OpenGL**

* **DrawSprocket**

#### GPU accelerated games tested

* **Diablo II** - All 4 Modes

* **Descent II 6500**

* **MechWarrior II 6500**

* **Quake 3 Demo**

* **Tomb Raider Last Revelation Demo**

* **Bugdom**

* **Unreal Tournament**

* **Unreal**

* **Myth II**

### OSX Guest Support

This MacEmu fork has full high-level emulation support for PowerPC OSX.

* **All PowerPC OSX Versions Supported** - 10.0 through 10.5

* **64 Bit OSX Support** - Through PowerPC 970, a.k.a. G5, emulation (osxcpu970 pref).

* **No ROM or Firmware Needed** - OSX is booted entirely through fake hooks and high level emulation and does not need a ROM or any other firmware to run.

* **Full ScreenSaver Support** - All ScreenSavers of all OSX versions are supported and run natively on the host at full speed provided you have the GPU to do so. Requires gfxaccel.

* **3D Game Support** - Alice Madness Returns, Giants Citizen Kabuto, Warcraft III, and Warcraft III Frozen Throne tested. Requires gfxaccel.

* **OSX x86+x64 Native JIT** - Since OSX is more demanding than classic it is recommended you turn on the OSX jit (osxjit).

* **Networking and Sound Support**

### PowerPC Guest Debugging Support

MacEmu now fully supports PowerPC debugging. CodeWarrior on classic mac and GCC and Clang on OSX are tested.

### Full Classic Joystick and Gamepad Support

Nearly every API ever released for classic is supported. The default build of MacEmu supports:

* **JoyManager** - This driver supports CH joysticks and a few others. Guest drivers are not needed for this.

* **Gravis FireBird** - Requires guest driver

* **ThrustMaster** - Requires guest driver

Optional compile-in backends include: Gravis Mousestick II, Gravis BlackHawk, Gravis GamePad, MacAlly gamepad, Microsoft SideWinder, and raw CH FlightStick Pro (though this is redundant with JoyManager).

USB is optionally supported as well, however its practicality is limited as it requires later versions of OS 9 and requires enabling the USB stack which can be intensive to emulate on classic.

### SoundBase Support

SoundBase, the pre-Sound Manager audio interface, is now supported for old games like Populous or Dark Castle

### CD Image and QCOW2 Support

MacEmu now support of variety of cd images: ISO, CUE+BIN, CHD, TOAST, and more.

MacEmu now also supports QCOW2 for hard drives.

Large 64-bit images are now supported including DVDs.

### Multi-CPU Support

MacEmu now supports multiple CPUs - generally with one host thread per CPU - for both classic and OSX. Tested 7.5.3, 7.6, 7.6.1, 8.0, 8.1, 8.5, 9.0.4 and 9.2.1, as well as all supported OSX versions.

### Automatic ROM Loading

If a ROM isn't specified and the classic OS has a ROM, that ROM is loaded instead.

### Improved Host<->Guest Clipboard Bridge

You can now copy and paste from the host to guest - and vice versa - more seamlessly. Including OSX.

### 9.2 Support

System 9.2 is now supported through either automatic ROM loading or the NewWorld 8.4 ROM.

### Emulator Monitor Remote Access Support

MacEmu now supports an "emulator monitor" through emumonitor.exe that can remotely send various commands to the emulator. These include file deployment, input - mouse and keyboard - commands, and much, much more.

### Printing to PDF Support (Experimental)

Some basic printing to PDF support is now included in MacEmu.

## PocketShaver Features

### JIT Compiler (Mac Catalyst)

A from-scratch PowerPC-to-arm64 dynamic recompiler, built as a native Apple Silicon backend for the existing Kheperix JIT framework:

- Threaded-code code generation with direct block chaining between compiled blocks
- Native arm64 translation of the integer ALU/logical/shift/rotate/multiply/divide/compare/condition-register ops, branches, integer and floating-point loads/stores, and FP arithmetic, with an inline fastmem path for guest memory access
- W^X code generation (`pthread_jit_write_with_callback_np`) for Apple Silicon's hardened runtime
- A default-deny mnemonic whitelist: only instructions with a validated, lockstep-tested native implementation compile natively -- everything else (AltiVec/vector ops included) falls back to the existing generic interpreter, so correctness never trades against coverage
- Toggle under **Advanced** preferences ("JIT compiler", restart required); on by default

JIT is only available on the Mac Catalyst build. Apple's JIT entitlements aren't available to iOS apps (including "Designed for iPad" on macOS), so those targets remain interpreter-only.

### On-Screen Gamepad (iOS / iPadOS)

A fully customizable virtual gamepad overlay for touchscreen play:

- Per-button assignment to keyboard keys, mouse clicks, or joystick types (mouse, WASD, arrows, 8-way)
- Configurable button grid layout
- Ordinary Classic Mac OS keyboard keys as well as special keys (Cmd-W, left click, right click, toggle audio etc.)
- Multiple saved configurations
- In-game editing mode for remapping buttons without leaving the emulator
- Example layouts included (arcade, FPS, RPG)

### Two-finger steering (iOS / iPadOS)

A new optional way of steering the mouse cursor on touchscreen that makes fast and accurate mouse control possible:

- **Accuracy** -- allows precise placement of cursor by not obscuring the cursor by your thumbs
- **Speed** -- allows quick placement of the cursor and mouse clicking at any place of the screen, by minimal thumb movement
- **Long drag** -- allows long mouse drag movements, for long click-and-drag actions or rectangular selection of a large area (as needed in real time strategy games)

### Touch Input (iOS / iPadOS)

- **Relative mouse mode** -- needed for certain game and software titles
- **Right-click** -- available as gamepad overlay key on touchscreen, as well as hardware support for physical mouse / mousepad
- **Soft keyboard** -- iOS keyboard bridged to emulated Mac input with configurable screen offset (top, middle, bottom)
- **Haptic feedback** -- independent toggles for gestures, mouse clicks, and key presses

### Bonjour LAN Networking

Peer-to-peer networking between devices over local network, using Bonjour:

- **Host mode** -- provides router functionality, shows connected clients
- **Client mode** -- discovers hosts via Bonjour, auto-join with persistent device tracking
- Device naming and renaming within the LAN
- Automatic reconnection after app suspension
- Networking between platforms also possible (iPhone to Mac etc.)
- Alternative Slirp networking also available

### Preferences

A tabbed preferences interface with five sections:

| Tab          | Contents                                                                                                                                    |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------- |
| **General**  | Setup, disk management (create/import/delete), audio toggle, input options, haptic feedback, hints                                          |
| **Graphics** | Monitor resolutions, rendering filter (nearest/bilinear), frame rate (60/75/120 Hz), gamma ramp, NQD/RAVE/GL acceleration toggles           |
| **Network**  | Slirp vs. Bonjour selection, host/client role, peer browsing, device naming                                                                 |
| **Advanced** | RAM setting, performance metrics (FPS counter), UI options, relative mouse settings, bootstrap/ROM info, JIT compiler toggle (Mac Catalyst) |

The UI adapts to the platform -- on macOS, the Gamepad tab is hidden and touch-specific options are suppressed.

### Disk and ROM Management

- Create new virtual disks with configurable size
- Import external disk images
- Install and validate Mac OS ROM files with version detection
- Boot disk selection and CD boot support

### Performance Monitoring

- FPS counter overlay option
- Network transfer rate overlay option

## Benchmarks

<p align="center">
  <img src="docs/images/macbench5.jpg" alt="MacBench 5.0 results for PocketShaver on Mac Catalyst, compared against a Power Macintosh G3/300" width="480">
</p>

<p align="center"><em>MacBench 5.0 scores (higher is better), normalized to a Power Macintosh G3/300 at 100&#37;. On Mac Catalyst, the PowerPC-to-arm64 JIT lifts Processor and Floating-Point past 6&times; the G3/300, while the four Metal graphics engines drive the Graphics, Publishing, Disk, and CD-ROM results.</em></p>

## Platform Support

| Platform                     | Status                                                               |
| ---------------------------- | -------------------------------------------------------------------- |
| iOS (iPhone/iPad)            | Primary target -- full touch, gamepad, and GPU acceleration          |
| "Designed for iPad" on macOS | Supported -- gamepad hidden, keyboard/mouse passthrough              |
| Mac Catalyst                 | Supported -- native Mac build with the PowerPC-to-arm64 JIT compiler |

## Building

PocketShaver is built as an Xcode project:

```
SheepShaver/src/MacOSX/PocketShaver.xcodeproj
```

Targets are included within the Xcode project for Mac Catalyst and iOS.

## Upstream

Forked from [kanjitalk755/macemu](https://github.com/kanjitalk755/macemu) (SheepShaver / BasiliskII). 
