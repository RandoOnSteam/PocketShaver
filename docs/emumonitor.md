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
| `ping` | Identify the monitor and port |
| `help` | Return the command list |
| `status` | Return guest width, height, and bit depth, plus the host window size |
| `shot PATH` | Save the guest framebuffer; `.png` paths are saved as PNG, anything else as BMP |
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
| `type TEXT` | Type US-layout ASCII text, using Shift where needed; `
` is Return, `	` is Tab, `\` is a backslash |
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

