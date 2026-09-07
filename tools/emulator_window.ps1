param(
    [ValidateSet('Capture', 'Click', 'Key')][string]$Action = 'Capture',
    [string]$Output = '',
    [int]$X = 0, [int]$Y = 0,
    [string]$Keys = ''
)
$ErrorActionPreference = 'Stop'
$emulator = Get-Process Ryujinx | Where-Object {
    $_.Path -eq 'D:\KRKR-ns-tools\emulator\publish\Ryujinx.exe' -and $_.MainWindowHandle -ne 0
} | Select-Object -First 1
if (-not $emulator) { throw 'Expected KRKR test emulator window not found' }
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KrkrTestWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
'@
[KrkrTestWindow]::SetForegroundWindow($emulator.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 350
if ([KrkrTestWindow]::GetForegroundWindow() -ne $emulator.MainWindowHandle) {
    throw 'Emulator did not take focus; refusing input or capture of another application'
}
$bounds = New-Object KrkrTestWindow+Rect
[KrkrTestWindow]::GetWindowRect($emulator.MainWindowHandle, [ref]$bounds) | Out-Null
switch ($Action) {
    'Click' {
        if ($X -lt 0 -or $Y -lt 0 -or $X -ge $bounds.Right-$bounds.Left -or $Y -ge $bounds.Bottom-$bounds.Top) {
            throw 'Click outside emulator window'
        }
        [KrkrTestWindow]::SetCursorPos($bounds.Left+$X, $bounds.Top+$Y) | Out-Null
        [KrkrTestWindow]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 70
        [KrkrTestWindow]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    }
    'Key' { [System.Windows.Forms.SendKeys]::SendWait($Keys) }
    'Capture' {
        if (-not $Output) { throw 'An explicit screenshot output path is required' }
        $bitmap = New-Object System.Drawing.Bitmap ($bounds.Right-$bounds.Left), ($bounds.Bottom-$bounds.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
            $bitmap.Save([System.IO.Path]::GetFullPath($Output), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
        Get-Item -LiteralPath $Output | Select-Object FullName, Length
    }
}
