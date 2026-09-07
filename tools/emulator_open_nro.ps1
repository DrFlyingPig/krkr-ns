param([Parameter(Mandatory=$true)][string]$Nro)
$ErrorActionPreference = 'Stop'
$file = Get-Item -LiteralPath $Nro
if ($file.Extension -ne '.nro') { throw 'Expected an NRO file' }
Add-Type -AssemblyName UIAutomationClient
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KrkrOpenDialog {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr window, uint msg, IntPtr wParam, string text);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
$emulator = Get-Process Ryujinx | Where-Object { $_.Path -eq 'D:\KRKR-ns-tools\emulator\publish\Ryujinx.exe' } | Select-Object -First 1
$root = [System.Windows.Automation.AutomationElement]::FromHandle($emulator.MainWindowHandle)
$dialog = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32770'))
if (-not $dialog) { throw 'Open the emulator file dialog first' }
$controls = $dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$edit = $controls | Where-Object { $_.Current.AutomationId -eq '1148' -and $_.Current.ClassName -eq 'Edit' } | Select-Object -First 1
$button = $controls | Where-Object { $_.Current.AutomationId -eq '1' -and $_.Current.ClassName -eq 'Button' } | Select-Object -First 1
if (-not $edit -or -not $button) { throw 'Expected file name and Open controls missing' }
[KrkrOpenDialog]::SendMessage([IntPtr]$edit.Current.NativeWindowHandle, 0x000C, [IntPtr]::Zero, $file.FullName) | Out-Null
[KrkrOpenDialog]::PostMessage([IntPtr]$button.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
