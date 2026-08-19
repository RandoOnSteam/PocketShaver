# Run SheepShaver in the scratch sandbox (its own copy of the boot image) for a
# bounded time, screenshot its window, then stop it and print what happened.
# Used to iterate on the USB work without touching the real disk images.
param([int]$Seconds = 40)

$src = "C:\Users\User\Documents\Shared\PocketShaver\out\build\x64-QD3D-RelWithDebInfo\SheepShaver"
$sb  = "C:\Users\User\AppData\Local\Temp\claude\C--Users-User-Documents-Shared-PocketShaver\57fbdef1-480a-472c-a41a-04322799fb4a\scratchpad\usbtest"

Copy-Item "$src\SheepShaver.exe" $sb -Force
Remove-Item "$sb\usbhid.log","$sb\err.log","$sb\out.log","$sb\screen.png" -ErrorAction SilentlyContinue

$p = Start-Process -FilePath "$sb\SheepShaver.exe" -WorkingDirectory $sb -PassThru `
        -RedirectStandardError "$sb\err.log" -RedirectStandardOutput "$sb\out.log"

$exited = $p.WaitForExit($Seconds * 1000)

if (-not $exited) {
    # Grab the emulator window so we can see what the guest is actually showing.
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
}
"@ -ErrorAction SilentlyContinue
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) {
        $r = New-Object W+R
        [void][W]::GetWindowRect($h, [ref]$r)
        $w = $r.Rt - $r.L; $ht = $r.B - $r.T
        if ($w -gt 0 -and $ht -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap $w, $ht
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
            $bmp.Save("$sb\screen.png", [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            "screenshot: $sb\screen.png ($w x $ht)"
        }
    } else { "no main window to capture" }

    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(5000)
    "STATUS: still running at ${Seconds}s (terminated)"
} else {
    "STATUS: exited on its own, code=$($p.ExitCode)"
}

"--- err.log (last 12) ---"
Get-Content "$sb\err.log" -Tail 12 -ErrorAction SilentlyContinue
"--- usbhid.log ---"
Get-Content "$sb\usbhid.log" -ErrorAction SilentlyContinue | Select-Object -Last 30
