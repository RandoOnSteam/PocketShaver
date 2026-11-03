
$drive = 'F:'
$path = "\\.\$drive"

Add-Type @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public class CdToc {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode,
        SetLastError=true)]
    public static extern SafeFileHandle CreateFile(
        string name, uint access, uint share,
        IntPtr security, uint creation,
        uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool DeviceIoControl(
        SafeFileHandle device, uint code,
        IntPtr input, uint inputSize,
        byte[] output, uint outputSize,
        out uint returned, IntPtr overlapped);
}
'@

# GENERIC_READ = 0x80000000, FILE_SHARE_READ|WRITE = 3, OPEN_EXISTING = 3
$handle = [CdToc]::CreateFile(
    $path, [uint32]"0x80000000", [uint32]3, [IntPtr]::Zero,
    [uint32]3, [uint32]0, [IntPtr]::Zero
)

if ($handle.IsInvalid) {
    throw "Cannot open drive: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

$buffer = New-Object byte[] 804
$bytesReturned = [uint32]0

# IOCTL_CDROM_READ_TOC
$ioctl = 0x00024000

$ok = [CdToc]::DeviceIoControl(
    $handle, $ioctl, [IntPtr]::Zero, 0,
    $buffer, $buffer.Length,
    [ref]$bytesReturned, [IntPtr]::Zero
)

if (!$ok) {
    throw "READ_TOC failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

$first = $buffer[2]
$last  = $buffer[3]

"First track: $first"
"Last track:  $last"
""


for ($i = 0; $i -lt ($last - $first + 1); $i++) {
    $offset = 4 + ($i * 8)

    $control = $buffer[$offset + 1] -band 0x0F
    $track = $buffer[$offset + 2]

    $minutes = [int]$buffer[$offset + 5]
    $seconds = [int]$buffer[$offset + 6]
    $frames  = [int]$buffer[$offset + 7]

    $lba = (($minutes * 60 + $seconds) * 75 + $frames) - 150

    $type = if ($control -band 0x04) { "DATA" } else { "AUDIO" }

    [PSCustomObject]@{
        Track   = $track
        Type    = $type
        Control = ('0x{0:X2}' -f $control)
        LBA     = $lba
    }
}

$handle.Dispose()
