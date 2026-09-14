# Run the built SheepShaver in its own build directory, with the real prefs and
# disk images, for a bounded time; screenshot the guest and stop it. Use this
# only to confirm the boot still reaches the Finder - the scratch sandbox in
# usbrun.ps1 is the right place for iterating.
param([int]$Seconds = 120)

$dir = "C:\Users\User\Documents\Shared\PocketShaver\out\build\x64-QD3D-RelWithDebInfo\SheepShaver"
Remove-Item "$dir\usbhid.log","$dir\real_err.log","$dir\real_out.log","$dir\real_screen.png" -ErrorAction SilentlyContinue

$p = Start-Process -FilePath "$dir\SheepShaver.exe" -WorkingDirectory $dir -PassThru `
        -RedirectStandardError "$dir\real_err.log" -RedirectStandardOutput "$dir\real_out.log"
$exited = $p.WaitForExit($Seconds * 1000)

if (-not $exited) {
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
}
"@ -ErrorAction SilentlyContinue
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) {
        [void][W2]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 700
        $r = New-Object W2+R
        [void][W2]::GetWindowRect($h, [ref]$r)
        $w = $r.Rt - $r.L; $ht = $r.B - $r.T
        if ($w -gt 0 -and $ht -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap $w, $ht
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
            $bmp.Save("$dir\real_screen.png", [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            "screenshot: $dir\real_screen.png ($w x $ht)"
        }
    } else { "no main window" }
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(5000)
    "STATUS: still running at ${Seconds}s (terminated)"
} else {
    "STATUS: exited on its own, code=$($p.ExitCode)"
}

"--- err.log (last 14) ---"
Get-Content "$dir\real_err.log" -Tail 14 -ErrorAction SilentlyContinue
"--- usbhid.log ---"
Get-Content "$dir\usbhid.log" -ErrorAction SilentlyContinue | Select-Object -Last 25
