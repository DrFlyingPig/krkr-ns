param([Parameter(Mandatory=$true)][string]$Nro)
$ErrorActionPreference = 'Stop'
$file = Get-Item -LiteralPath $Nro
if ($file.Extension -ne '.nro') { throw 'Expected an NRO file' }
Add-Type -AssemblyName UIAutomationClient
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class KrkrOpenDialog {
    public delegate bool EnumProc(IntPtr window, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    public static IntPtr FindDialog(uint process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, param) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            var name = new StringBuilder(128); GetClassName(window, name, name.Capacity);
            if (owner == process && name.ToString() == "#32770") { result = window; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr window, uint msg, IntPtr wParam, string text);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
$emulator = Get-Process Ryujinx | Where-Object { $_.Path -eq 'D:\KRKR-ns-tools\emulator\publish\Ryujinx.exe' } | Select-Object -First 1
$dialogHandle = [KrkrOpenDialog]::FindDialog($emulator.Id)
if ($dialogHandle -eq [IntPtr]::Zero) { throw 'Open the emulator file dialog first' }
$dialog = [System.Windows.Automation.AutomationElement]::FromHandle($dialogHandle)
$controls = $dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$edit = $controls | Where-Object { $_.Current.AutomationId -eq '1148' -and $_.Current.ClassName -eq 'Edit' } | Select-Object -First 1
$button = $controls | Where-Object { $_.Current.AutomationId -eq '1' -and $_.Current.ClassName -eq 'Button' } | Select-Object -First 1
if (-not $edit -or -not $button) { throw 'Expected file name and Open controls missing' }
[KrkrOpenDialog]::SendMessage([IntPtr]$edit.Current.NativeWindowHandle, 0x000C, [IntPtr]::Zero, $file.FullName) | Out-Null
[KrkrOpenDialog]::PostMessage([IntPtr]$button.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
