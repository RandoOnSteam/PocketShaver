# emumonitor

The emulator monitor lets scripts drive a running Basilisk II or PocketShaver (SheepShaver) from outside: take screenshots, move the guest mouse, type, press keys, copy programs into the guest, read files from the guest's disks, and quit the emulator.

- [Building](#building)
- [Clients](#clients)
- [Protocol](#protocol)
- [Guest commands](#guest-commands)
- [Host commands](#host-commands)
- [Deploying guest programs](#deploying-guest-programs)
- [Reading guest files](#reading-guest-files)
- [Quitting the emulator](#quitting-the-emulator)
- [Recipes](#recipes)
- [Troubleshooting](#troubleshooting)

## Building

The monitor is disabled by default and is compiled in with:

```text
-DENABLE_EMULATOR_MONITOR=ON
```

That builds the server into the emulator and also builds the native client, `emumonitor` (`emumonitor.exe` on Windows), from `tools/emumonitor.c`.

The server listens on TCP port `19840`.
On desktop targets it binds to loopback (`127.0.0.1`) only, so it cannot be reached from another machine.
On iOS it binds to every interface, so it can be reached over the device's network.

The server starts once the emulator's window is up.
If the port is already taken, for example by a second emulator, that emulator runs without a monitor and says nothing; only the first one answers.

## Clients

| Client | Monitor commands | `deploy`, `find`, `cat`, `fetch` | `quit` that verifies and falls back to killing |
| --- | --- | --- | --- |
| `emumonitor` (native, built by CMake) | yes | yes | yes |
| `tools/emumonitor.ps1` | yes | no | no, sends the monitor `quit` only |
| `tools/emumonitor.py` | yes | no | no, sends the monitor `quit` only |

All three take `--host` and `--port` (`-HostName` and `-Port` for PowerShell), defaulting to `127.0.0.1` and `19840`.
Everything after the options is joined with single spaces and sent as one command.

```text
emumonitor status
emumonitor --host 192.168.1.20 status
```

The server port is always 19840; `--port` is for reaching it through a forwarded port, for example an SSH tunnel to a desktop emulator on another machine:

```text
ssh -N -L 29840:127.0.0.1:19840 me@macmini &
emumonitor --port 29840 status
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 status
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 shot C:\temp\screen.png
powershell -ExecutionPolicy Bypass -File tools\emumonitor.ps1 -HostName 192.168.1.20 click 20 12
```

```text
python3 tools/emumonitor.py status
python3 tools/emumonitor.py --host 192.168.1.20 shot screen.png
```

Each client prints the monitor's JSON reply on one line.

Native client exit codes:

| Code | Meaning |
| --- | --- |
| 0 | The command worked (`"ok":true`) |
| 1 | The monitor answered `"ok":false`, or `find` matched nothing |
| 2 | Bad arguments, no connection, no reply, or a local file error |

The PowerShell and Python clients exit with 1 on `"ok":false`; a connection failure shows up as an exception.

## Protocol

The protocol is plain text over TCP, so any language or `nc` can talk to it:

```text
printf 'status\n' | nc 127.0.0.1 19840
{"ok":true,"width":800,"height":600,"depth":32,"hostwidth":800,"hostheight":600}
```

- Send one command per connection, ending in a newline. The server reads up to 1023 bytes or the first newline, strips trailing spaces, tabs, and line endings, and runs it.
- The server replies with one JSON line and closes the connection.
- Success replies contain `"ok":true`. Failures are `{"ok":false,"error":"MESSAGE"}`, for example `unknown command`, `missing path`, `button must be 0, 1, or 2`, `key code must be 0 through 127`, `unsupported character`, or `extfs is not enabled`.
- Only one client is served at a time; others wait in the listen queue.
- `ping`, `help`, `extfs`, and `disks` are answered by the monitor thread itself, so they still work when the emulator's window loop is stuck.
- Every other command is handed to the emulator's SDL event loop and the reply is sent once the loop has run it. If the loop is stuck, these commands never answer. The native client then waits forever, except in `quit`, which has its own timeouts.
- Commands that take time, such as `click`, `drag`, `hold`, `key tap`, and `type`, reply only after they finish, so a script can send the next command straight away.

## Guest commands

Guest commands go straight to the emulated ADB mouse and keyboard and never touch the host mouse or keyboard.
Guest coordinates are Mac framebuffer pixels, with `0 0` at the top-left of the Mac screen, matching `status` `width` and `height` and the pixels in a `shot`.

| Command | Effect |
| --- | --- |
| `ping` | Identify the monitor, port, and emulator process id |
| `help` | Return the command list |
| `status` | Return guest width, height, and bit depth, plus the host window size |
| `extfs` | Return the host folder shown in the guest as the extfs volume |
| `disks` | Return the extfs volume name and the absolute path of every `disk` preference |
| `quit` | Inject the hotkey + Esc combination, the emulator's emergency quit, which exits without asking the guest |
| `shot PATH` | Save the guest framebuffer, read straight from guest memory so it also works with gfxaccel; `.png` paths are saved as PNG, anything else as BMP |
| `mouse X Y` | Send an ADB mouse movement, absolute or relative depending on the active mouse mode |
| `mousedown BUTTON` | Press ADB mouse button 0, 1, or 2 |
| `mouseup BUTTON` | Release ADB mouse button 0, 1, or 2 |
| `click X Y [BUTTON]` | Move, wait 50 ms, press, wait 60 ms, and release |
| `dblclick X Y [BUTTON]` | Move and click twice, 60 ms apart, well inside the double-click time |
| `drag X1 Y1 X2 Y2 [BUTTON [HOLDMS]]` | Press at the start, move in 10 steps 20 ms apart, wait HOLDMS (default 60), and release |
| `hold X Y MS [BUTTON]` | Move, press, keep the button down for MS, and release |
| `key down CODE` | Press an ADB key code |
| `key up CODE` | Release an ADB key code |
| `key tap CODE` | Press, wait 60 ms, and release |
| `type TEXT` | Type US-layout ASCII text, using Shift where needed; `\n` is Return, `\t` is Tab, `\\` is a backslash |
| `power` | Tap the ADB power key (0x7f) |

`BUTTON` defaults to 0, the main button.

### Replies

```text
> ping
{"ok":true,"monitor":"BasiliskII","port":19840,"pid":4120}
> status
{"ok":true,"width":640,"height":480,"depth":8,"hostwidth":1280,"hostheight":960}
> extfs
{"ok":true,"path":"C:\\Emulators\\BasiliskII\\Virtual Desktop"}
> disks
{"ok":true,"extfsname":"My Computer","disks":["C:\\Emulators\\MacOS9HD.dsk","C:\\Emulators\\Apps.dsk"]}
> shot C:\temp\screen.png
{"ok":true,"path":"C:\\temp\\screen.png"}
> click 20 12
{"ok":true}
> frobnicate
{"ok":false,"error":"unknown command"}
```

PocketShaver answers `ping` with `"monitor":"PocketShaver"`.
Paths in replies use JSON escaping, so Windows backslashes appear doubled.

### Screenshots

```text
emumonitor shot screen.png
emumonitor shot C:\temp\guest.bmp
emumonitor shot /tmp/guest.png
```

The emulator process writes the file, not the client.
A relative path is resolved against the emulator's working folder, and with `--host` the file is written on the emulator's machine.
Use absolute paths in scripts.

### Mouse

```text
emumonitor click 20 12             # click the Apple menu at the top left
emumonitor dblclick 600 60         # open a desktop icon
emumonitor click 320 240 1         # click with button 1
emumonitor hold 20 12 1500         # hold the Apple menu open for 1.5 s
emumonitor drag 40 60 400 300      # drag an icon
emumonitor drag 230 9 240 140 0 300  # pull down a menu, hover 300 ms, release on an item
```

Hand-built press, move, and release, for example to choose Special > Shut Down from the System 7 Finder:

```text
emumonitor mouse 232 9
emumonitor mousedown 0
emumonitor mouse 240 100
emumonitor mouse 245 139
emumonitor mouseup 0
```

When the guest has switched the ADB mouse to relative mode, such as games that capture the mouse, `mouse X Y` is sent as a relative movement of X, Y.

### Keyboard

`CODE` is an ADB key code, given in decimal, hex (`0x35`), or octal (a leading `0`, so `010` is 8).
`type` covers printable US ASCII; use `key` for everything else.

Common ADB key codes:

| Key | Code | Key | Code |
| --- | --- | --- | --- |
| Return | `0x24` | Command | `0x37` |
| Tab | `0x30` | Shift | `0x38` |
| Space | `0x31` | Caps Lock | `0x39` |
| Delete (backspace) | `0x33` | Option | `0x3a` |
| Esc | `0x35` | Control | `0x36` |
| Forward delete | `0x75` | Left arrow | `0x3b` |
| F1 | `0x7a` | Right arrow | `0x3c` |
| Power | `0x7f` | Down arrow | `0x3d` |
| | | Up arrow | `0x3e` |

Letters follow the Mac layout, not alphabetical order: A `0x00`, S `0x01`, D `0x02`, F `0x03`, H `0x04`, G `0x05`, Z `0x06`, X `0x07`, C `0x08`, V `0x09`, B `0x0b`, Q `0x0c`, W `0x0d`, E `0x0e`, R `0x0f`, Y `0x10`, T `0x11`, O `0x1f`, U `0x20`, I `0x22`, P `0x23`, L `0x25`, J `0x26`, K `0x28`, N `0x2d`, M `0x2e`.

Key combinations are built with `key down` and `key up`:

```text
emumonitor key down 0x37           # Command-O: open the selected item
emumonitor key tap 0x1f
emumonitor key up 0x37

emumonitor key down 0x37           # Command-Option-Esc: force quit
emumonitor key down 0x3a
emumonitor key tap 0x35
emumonitor key up 0x3a
emumonitor key up 0x37
```

Typing text:

```text
emumonitor type hello world
emumonitor type "Hello, World!"
emumonitor type "line one\nline two\n"
emumonitor type "C:\\path"           # types C:\path
```

The native client joins its arguments with spaces, so the quotes are only needed for the shell.
In bash, put text with `\n`, `!`, or `$` in single quotes so the shell leaves it alone: `emumonitor type 'cd ~\n'`.
Each character takes about 60 ms, so a long `type` takes a while to reply.
A character outside the table stops typing at that point and returns `unsupported character`; the characters before it have already been typed.

## Host commands

Host commands go through the emulator's normal SDL event path as if they came from the host, so window scaling, letterboxing, and mouse grab are exercised.
They are injected into the emulator's event queue and never move the real host cursor.
Host coordinates are window client pixels in the emulator's own coordinate space, which is the same size as `host shot`.

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

Comparing `shot` and `host shot` shows whether a problem is in the guest image or in the host's scaling and presentation:

```text
emumonitor shot C:\temp\guest.png
emumonitor host shot C:\temp\window.png
emumonitor host click 640 480      # click the middle of a 1280x960 window
emumonitor hotkey fullscreen
```

## Deploying guest programs

`deploy` copies a MacBinary file into a folder that the guest sees through extfs, so a program built on the host can be run in the guest right away.
It is a native-client command; the emulator does not take part except to say where its extfs folder is.

```text
emumonitor deploy FILE.bin [FOLDER]
```

### Sources

| Source | Example | How it is read |
| --- | --- | --- |
| Local file | `HelloWorld.bin`, `build\HelloWorld.bin`, `C:\out\HelloWorld.bin`, `/home/me/HelloWorld.bin` | Read directly |
| scp host and path | `user@buildhost:/path/HelloWorld.bin`, `buildhost:build/HelloWorld.bin` | Copied with `scp -q` first |
| scp URL | `scp://user@buildhost:2200/path/in/home/HelloWorld.bin` | Copied with `scp -q` first; use this form for a non-standard port |

A source counts as remote only when no local file has that name and it contains a `:` after its second character, so Windows drive paths such as `C:\out\x.bin` are always local.
Remote sources need an `scp` on the `PATH` (Windows 10 and later include OpenSSH) and key-based login, because `scp` cannot ask for a password through the client.
The copy goes to `emumonitor-deploy.bin` in `%TEMP%`, `$TMPDIR`, or `/tmp`, is overwritten by the next remote deploy, and is not deleted.

### Destination

| Command | Destination |
| --- | --- |
| `emumonitor deploy X.bin` | The running emulator's extfs folder, from the monitor's `extfs` reply |
| `emumonitor deploy X.bin FOLDER` | `FOLDER`, which must already exist; the emulator does not have to be running |

The extfs folder is:

- Windows: the `Virtual Desktop` folder next to the emulator executable, whenever `enableextfs` is on. The guest shows its contents at the root of the extfs volume, normally named `My Computer`.
- Everywhere else: the folder in the `extfs` preference. The guest shows it as the `Unix` volume.

If extfs is off, `deploy` without a folder fails with `extfs is not enabled`.
Deploying into a subfolder makes the program appear inside that folder in the guest:

```text
emumonitor deploy HelloWorld.bin "C:\Emulators\BasiliskII\Virtual Desktop\Tests"
emumonitor deploy HelloWorld.bin ~/macshare/Tests
```

Give an explicit folder when the emulator was started from somewhere unexpected, when it is not running yet, or when the extfs folder is reached through a share or mount under a different path.

### What deploy writes

`deploy` checks the MacBinary header, then splits the file into the three files extfs reads, using the name, type, and creator stored in the header:

```text
FOLDER/
  NAME              data fork
  .rsrc/NAME        resource fork
  .finf/NAME        32 bytes of Finder info
```

- `.rsrc` and `.finf` are created if missing.
- Existing files with the same name are replaced.
- Every fork is written, even an empty one, so a stale resource fork from an earlier version never survives.
- The Finder info keeps the type, creator, and Finder flags from the header. The "has been inited" flag is cleared and the icon position is set to "let the Finder choose", so the Finder registers the program's icon and bundle and places it in a free spot.
- MacBinary I, II, and III files are accepted. The header must have zero bytes at offsets 0 and 74 and a name of 1 to 63 characters. Files that fail are reported as `X is not a MacBinary file`, and files shorter than their header claims as `X is truncated`.

Mac names are turned into host names the same way extfs does it: control characters, characters from 0x80 up, and any of `/ \ : * ? " < > | %` become `%XX` with the hex code.
So `Résumé` becomes `R%8Esum%8E`, and `Test/Build` becomes `Test%2FBuild`.
The guest shows the original Mac name.

On success the client prints the host name and the fork sizes:

```text
{"ok":true,"name":"HelloWorld","data":0,"rsrc":4321}
```

On failure it prints a message to standard error, such as `Unable to read X`, `Unable to fetch X`, `Unable to write X`, or `Unable to get extfs folder`, and exits with 2.

### Getting a MacBinary file

- Retro68 writes `NAME.bin` (MacBinary) next to `NAME.APPL`, plus `NAME.dsk` with the program on a disk image. Deploy the `.bin`.
- `emumonitor fetch` saves any file from the guest's disks as MacBinary II, so files can be moved between emulators or kept on the host and deployed again later.
- Other tools that write MacBinary also work, for example `macbinary encode` from macutils, or StuffIt and BinHex 5 in the guest.

### Examples

Deploy a local build into the running emulator:

```text
emumonitor deploy build\HelloWorld.bin
```

Deploy straight from a Linux build machine:

```text
emumonitor deploy me@buildbox:Retro68-build/hello/build/HelloWorld.bin
emumonitor deploy scp://me@192.168.40.116:2200/hello/build/HelloWorld.bin
```

Deploy before starting the emulator, into the folder it will share:

```text
emumonitor deploy HelloWorld.bin "C:\Emulators\BasiliskII\Virtual Desktop"
BasiliskII.exe
```

Deploy into another emulator's share on the same machine without going through its monitor:

```text
emumonitor deploy HelloWorld.bin "D:\SheepShaver\Virtual Desktop"
```

Move a program from the running emulator's disk into another emulator's shared folder:

```text
emumonitor fetch "Macintosh HD:Applications:SimpleText" SimpleText.bin
emumonitor deploy SimpleText.bin "D:\SheepShaver\Virtual Desktop"
```

### Edit, build, deploy, run

Quit the program in the guest before deploying it again, because an open file cannot be replaced.
The Finder does not notice files that appear behind its back while a window shows the folder, so close the window first and reopen it afterwards.
A full loop, in PowerShell, for a program shown in a Finder window that is frontmost:

```powershell
$m = "emumonitor"
& $m key down 0x37; & $m key tap 0x0c; & $m key up 0x37   # Command-Q in the program
Start-Sleep 1
& $m key down 0x37; & $m key tap 0x0d; & $m key up 0x37   # Command-W closes the Finder window
& $m deploy me@buildbox:hello/build/HelloWorld.bin
if ($LASTEXITCODE -ne 0) { exit 1 }
& $m type "HelloWorld"                                    # select it by typing its name
& $m key down 0x37; & $m key tap 0x1f; & $m key up 0x37   # Command-O opens it
Start-Sleep 3
& $m shot C:\temp\hello.png
```

The same loop in bash:

```bash
m=emumonitor
cmd() { $m key down 0x37; $m key tap "$1"; $m key up 0x37; }
cmd 0x0c; sleep 1                      # Command-Q
cmd 0x0d                               # Command-W
$m deploy me@buildbox:hello/build/HelloWorld.bin || exit 1
$m type HelloWorld
cmd 0x1f                               # Command-O
sleep 3
$m shot /tmp/hello.png
```

Typing a name selects the matching icon on the desktop or in the frontmost Finder window, which is more reliable than clicking at fixed coordinates.
If the program sits at the root of the extfs volume, open that volume first, for example by double-clicking its icon or by typing its name on the desktop and pressing Command-O.

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
The client opens the reported paths on its own machine, so with `--host` they only work when the client sees the same paths.

- `find PATTERN [VOLUME:FOLDER]` matches file and folder names case-insensitively. `*` and `?` are wildcards; a pattern without them matches anywhere in the name. Each match is printed as a full Mac path, and files add the type, creator, data fork size, and resource fork size, separated by tabs. Folders end in `:`. The exit code is 1 when nothing matches.
- `cat VOLUME:PATH` prints the data fork as text, turning carriage returns into line feeds and MacRoman into UTF-8.
- `fetch VOLUME:PATH [FILE.bin]` saves both forks and the Finder info as a MacBinary II file, the same format `deploy` reads. Without a file name it writes `NAME.bin` in the current folder, with the name escaped the same way as `deploy` does. It prints `{"ok":true,"path":"FILE.bin","data":N,"rsrc":N}`.

Paths use `:` between names and start with the volume name, as the Finder shows it.
Names may contain non-ASCII characters; give them in UTF-8 on the command line.
The guest caches disk writes, so a file saved moments ago can read as stale or partly written until Mac OS flushes the volume.

```text
emumonitor find jitbench
emumonitor find "*.log"
emumonitor find "?ystem" "MacOS9:"
emumonitor cat "MacOS9:jitbench.log"
emumonitor fetch "MacOS9:Applications:SimpleText"
```

Sample `find` output, with folders ending in `:` and files followed by type, creator, data size, and resource size:

```text
MacOS9:Benchmarks:
MacOS9:Benchmarks:jitbench	APPL	????	0	48112
MacOS9:Benchmarks:jitbench.log	TEXT	ttxt	211	0
```

## Quitting the emulator

```text
emumonitor quit
emumonitor quit 10000
```

The native client's `quit` gets the emulator's process id with `ping`, sends the monitor `quit`, and waits up to the timeout (5000 ms by default) for the process to exit.
If it is still running, or the monitor stops answering, the client ends it with `TerminateProcess` on Windows or `SIGKILL` elsewhere.
If the monitor does not answer `ping` within 2 seconds, the emulator is treated as hung: the client finds the process that owns the monitor's listening port and kills it right away.
On Windows it loads `GetExtendedTcpTable` (XP SP2 and later) or `AllocateAndGetTcpExTableFromStack` (XP) from `iphlpapi.dll` at run time; older Windows has neither, so there only the monitor `quit` and the timed kill of a known pid are available. On Linux it reads `/proc/net/tcp` and each process's socket descriptors, and on macOS it uses `libproc`.
It prints `"method":"quit"` or `"method":"kill"` to show which one worked:

```text
{"ok":true,"pid":4120,"method":"quit"}
{"ok":true,"pid":4120,"method":"kill"}
```

A killed emulator does not flush its disk images, so the guest may check its disks on the next boot.
With a remote `--host` only the monitor `quit` is sent, since the process cannot be ended from another machine, and the reply says `"verified":false`.

The monitor `quit`, like the hotkey + Esc it injects, also skips the guest's shutdown.
To shut the guest down cleanly, use its own Shut Down command and wait for the process to exit:

```text
emumonitor mouse 232 9
emumonitor mousedown 0
emumonitor mouse 240 100
emumonitor mouse 245 139
emumonitor mouseup 0
```

These coordinates are for the System 7 Finder's Special menu at 640x480; take a `shot` to find them on other systems and screen sizes.
`power` taps the ADB power key, which on Mac OS 8 and 9 normally brings up the Restart / Sleep / Shut Down dialog; then `key tap 0x24` presses its default button, Shut Down.

## Recipes

Wait for the emulator to come up, then for the Finder:

```powershell
do { Start-Sleep 1; emumonitor ping *> $null } while ($LASTEXITCODE -ne 0)
Start-Sleep 30
emumonitor shot C:\temp\boot.png
```

```bash
until emumonitor ping >/dev/null 2>&1; do sleep 1; done
sleep 30
emumonitor shot /tmp/boot.png
```

`ping` answers as soon as the window is up, long before the Finder; poll `shot` and compare images if a script needs to know the desktop is ready.

Read one field of a reply:

```powershell
$s = emumonitor status | ConvertFrom-Json
"$($s.width)x$($s.height) at $($s.depth) bits"
```

```bash
emumonitor status | python3 -c 'import json,sys; s=json.load(sys.stdin); print(s["width"], s["height"])'
```

Stop on the first failure in a bash script:

```bash
set -e
emumonitor click 20 12
emumonitor key tap 0x24
```

Run a benchmark program and collect its log from the guest's disk without touching the guest:

```bash
emumonitor deploy jitbench.bin
emumonitor type jitbench
emumonitor key down 0x37; emumonitor key tap 0x1f; emumonitor key up 0x37
until emumonitor cat "My Computer:jitbench.log" 2>/dev/null | grep -q "jitbench done"; do sleep 2; done
emumonitor cat "My Computer:jitbench.log"
```

Use the Python client from another program:

```python
import json, socket

def Monitor(command, host="127.0.0.1", port=19840):
    with socket.create_connection((host, port), timeout=30) as connection:
        connection.sendall((command + "\n").encode("ascii"))
        reply = b""
        while not reply.endswith(b"\n"):
            block = connection.recv(4096)
            if not block:
                break
            reply += block
    return json.loads(reply)

print(Monitor("status"))
Monitor("click 20 12")
```

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `Unable to connect to 127.0.0.1:19840` | The emulator is not running, was built without `ENABLE_EMULATOR_MONITOR`, has not opened its window yet, or another emulator owns the port. |
| Commands other than `ping` hang | The emulator's event loop is stuck. `emumonitor quit` still works because it falls back to killing the process. |
| `{"ok":false,"error":"unknown command"}` | A typo, a missing argument, or an argument that is not a number. |
| `shot` file is missing | The path was relative to the emulator's working folder, or the file was written on the `--host` machine. |
| Clicks land in the wrong place | Guest coordinates are Mac pixels, not window pixels; use `host click` for window pixels. In relative mouse mode `mouse` moves by X, Y instead. |
| `type` stops with `unsupported character` | The text has a character outside printable US ASCII; send it with `key`. |
| `deploy` says `extfs is not enabled` | Turn on extfs in the preferences, or pass the folder explicitly. |
| `deploy` says `Unable to write` | The program is still open in the guest, the folder does not exist, or the folder is read-only. |
| `deploy` says `Unable to fetch` | `scp` is missing, cannot log in without a password, or the remote path is wrong. Try the same `scp` command by hand. |
| Deployed program does not appear | The Finder window was open during the deploy; close it and open it again. |
| Deployed program shows a generic icon | The Finder has not rebuilt its desktop database for the new bundle yet; open the program once, or rebuild the desktop. |
| `find` or `cat` reads old contents | The guest has not flushed the disk yet; wait, or do something that makes Mac OS write, such as closing the file. |
