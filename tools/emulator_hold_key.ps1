param(
    [Parameter(Mandatory=$true)][string]$Key,
    [int]$HoldMs = 400
)
# Sends a real key-down / key-up pair to the KRKR test emulator window.  The
# emulator's keyboard controller only deflects an analog stick while the key is
# held, so a plain SendKeys press (down+up within milliseconds) can fall between
# two guest frames and never be sampled.  This holds the key long enough for the
# port's per-frame gamepad poll to see it.
$ErrorActionPreference = 'Stop'
$emulator = Get-Process Ryujinx -ErrorAction Stop | Where-Object {
    $_.Path -eq 'D:\KRKR-ns-tools\emulator\publish\Ryujinx.exe' -and $_.MainWindowHandle -ne 0
} | Select-Object -First 1
if (-not $emulator) { throw 'Expected KRKR test emulator window not found' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KrkrHoldKey {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
'@
[KrkrHoldKey]::SetForegroundWindow($emulator.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
if ([KrkrHoldKey]::GetForegroundWindow() -ne $emulator.MainWindowHandle) {
    throw 'Emulator did not take focus; refusing to send keys to another application'
}
$vk = [int][char]([string]$Key).ToUpper()
if ([string]$Key -eq 'Up') { $vk = 0x26 }
elseif ([string]$Key -eq 'Down') { $vk = 0x28 }
elseif ([string]$Key -eq 'Left') { $vk = 0x25 }
elseif ([string]$Key -eq 'Right') { $vk = 0x27 }
[KrkrHoldKey]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds $HoldMs
[KrkrHoldKey]::keybd_event([byte]$vk, 0, 2, [UIntPtr]::Zero)
Write-Output ("held {0} (vk 0x{1:X2}) for {2}ms" -f $Key, $vk, $HoldMs)