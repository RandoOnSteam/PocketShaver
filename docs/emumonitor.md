# emumonitor

The emulator monitor is disabled by default and is compiled in with:

```text
-DENABLE_EMULATOR_MONITOR=ON
```

It listens on TCP port `19840`, using loopback on desktop targets and the device network interfaces on iOS targets.

Native client built by CMake:

```text
emumonitor status
emumonitor shot screen.png
emumonitor click 20 12 0
emumonitor key tap 0x35
emumonitor --host 192.168.1.20 status
```

Windows PowerShell client:

```powershell
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 status
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 shot C:\temp\screen.png
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 click 20 12 0
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 key tap 0x35
```

Python client:

```text
python3 tools/emumonitor.py status
python3 tools/emumonitor.py --host 192.168.1.20 shot screen.png
```

The protocol accepts one newline-terminated command per TCP connection and returns one JSON line.

Guest commands go straight to the emulated ADB devices and never touch the host mouse or keyboard.
Guest coordinates are Mac framebuffer pixels.

| Command | Effect |
| --- | --- |
| `ping` | Identify the monitor, port, and emulator process id |
| `help` | Return the command list |
| `status` | Return guest width, height, and bit depth, plus the host window size |
| `extfs` | Return the host folder shown in the guest as the extfs volume |
| `disks` | Return the extfs volume name and the absolute path of every `disk` preference |
| `quit` | Inject the hotkey + Esc combination, the emulator's emergency quit, which exits without asking the guest |
| `shot PATH` | Save the guest framebuffer, read straight from guest memory so it also works with gfxaccel; `.png` paths are saved as PNG, anything else as BMP |
| `mouse X Y` | Send an absolute or relative ADB mouse movement, following the active mouse mode |
| `mousedown BUTTON` | Press ADB mouse button 0, 1, or 2 |
| `mouseup BUTTON` | Release ADB mouse button 0, 1, or 2 |
| `click X Y [BUTTON]` | Move, press, wait 60 ms, and release |
| `dblclick X Y [BUTTON]` | Move and click twice within the double-click time |
| `drag X1 Y1 X2 Y2 [BUTTON [HOLDMS]]` | Press at the start, move in 10 steps, wait HOLDMS (default 60), and release |
| `hold X Y MS [BUTTON]` | Move, press, keep the button down for MS, and release |
| `key down CODE` | Press an ADB key code |
| `key up CODE` | Release an ADB key code |
| `key tap CODE` | Press, wait 60 ms, and release |
| `type TEXT` | Type US-layout ASCII text, using Shift where needed; `\n` is Return, `\t` is Tab, `\\` is a backslash |
| `power` | Tap the ADB power key |

Host commands go through the emulator's normal SDL event path as if they came from the host, so window scaling, letterboxing, and mouse grab are exercised.
They are injected into the emulator's event queue and never move the real host cursor.
Host coordinates are window client pixels in the emulator's own coordinate space, which is the same size as "host shot".

| Command | Effect |
| --- | --- |
| `host shot PATH` | Save what the window shows after scaling; SDL 1.2 saves the window surface, SDL 2 and SDL 3 on Windows capture the window with `PrintWindow` |
| `host mouse X Y` | Inject a host mouse motion event |
| `host mousedown BUTTON` | Inject a host mouse button press |
| `host mouseup BUTTON` | Inject a host mouse button release |
| `host click X Y [BUTTON]` | Inject a host move, press, and release |
| `hotkey fullscreen` | Inject the hotkey + Return combination that toggles fullscreen |
| `hotkey grab` | Inject the hotkey + F5 combination that toggles mouse grab |

The hotkey modifiers follow the `hotkey` preference.

## Deploying guest programs

The native client can copy a MacBinary file into the extfs volume, so a program built on the host can be run in the guest right away:

```text
emumonitor deploy HelloWorld.bin
emumonitor deploy user@buildhost:/path/HelloWorld.bin
emumonitor deploy scp://user@buildhost:2200/path/in/home/HelloWorld.bin
emumonitor deploy HelloWorld.bin "C:\SheepShaver\Virtual Desktop"
```

`deploy` splits the file into the data fork, `.rsrc`, and `.finf` files that extfs reads, using the name, type, and creator stored in the MacBinary header.
Without a folder it asks the running emulator with `extfs`; on Windows this is the `Virtual Desktop` folder next to the emulator, which the guest shows at the root of the extfs volume, and elsewhere it is the `extfs` preference.
Remote sources are fetched with `scp` first.
Retro68 writes the MacBinary file as `NAME.bin` next to the `.APPL`.
If the Finder already has the folder open, close and reopen the window to see a new file.
Quit the program in the guest before deploying it again, because an open file cannot be replaced.

## Quitting the emulator

```text
emumonitor quit
emumonitor quit 10000
```

The native client's `quit` gets the emulator's process id with `ping`, sends the monitor `quit`, and waits up to the timeout (5000 ms by default) for the process to exit.
If it is still running, or the monitor stops answering, the client ends it with `TerminateProcess` on Windows or `SIGKILL` elsewhere.
If the monitor does not answer `ping` within 2 seconds, the emulator is treated as hung: the client finds the process that owns the monitor's listening port and kills it right away. On Windows it loads `GetExtendedTcpTable` (XP SP2 and later) or `AllocateAndGetTcpExTableFromStack` (XP) from `iphlpapi.dll` at run time; older Windows has neither, so there only the monitor `quit` and the timed kill of a known pid are available. On Linux `/proc/net/tcp` and each process's socket descriptors, and on macOS `libproc`.
It prints `"method":"quit"` or `"method":"kill"` to show which one worked.
A killed emulator does not flush its disk images, so the guest may check its disks on the next boot.
With a remote `--host` only the monitor `quit` is sent, since the process cannot be ended from another machine.

## Reading guest files

The native client can also search and read the guest's disks from the host:

```text
emumonitor find pdcmac
emumonitor find "*.c" "bvol:PDCursesMod:mac"
emumonitor cat "bvol:PDCursesMod:mac:pdcmac.h"
emumonitor fetch "MacOS9:System Folder:Finder" Finder.bin
```

All three ask the running emulator with `disks`, which returns the extfs volume name and the absolute path of every `disk` preference.
They then read the HFS and HFS+ disk images directly, including images with an Apple partition map, embedded HFS+ wrappers, and DiskCopy 4.2 headers, plus the extfs folder.
Nothing runs in the emulator, so it is not slowed down; on Windows the emulator opens disk images with read sharing so the client can read them while they are mounted.

- `find PATTERN [VOLUME:FOLDER]` matches file and folder names case-insensitively. `*` and `?` are wildcards; a pattern without them matches anywhere in the name. Each match is printed as a full Mac path, and files add the type, creator, data fork size, and resource fork size, separated by tabs. Folders end in `:`. The exit code is 1 when nothing matches.
- `cat VOLUME:PATH` prints the data fork as text, turning carriage returns into line feeds and MacRoman into UTF-8.
- `fetch VOLUME:PATH [FILE.bin]` saves both forks and the Finder info as a MacBinary II file, the same format `deploy` reads. Without a file name it writes `NAME.bin` in the current folder.

Paths use `:` between names and start with the volume name, as the Finder shows it.
The guest caches disk writes, so a file saved moments ago can read as stale or partly written until Mac OS flushes the volume.

