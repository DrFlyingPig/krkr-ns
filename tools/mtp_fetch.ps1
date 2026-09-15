param(
    [string]$SubPath = 'switch/KRKR-ns/log',
    [string]$OutDir = "$env:TEMP\krkr-device",
    [int]$Newest = 0,
    [string]$Name = '',
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$OutDir = $OutDir -replace '/', '\'
$shell = New-Object -ComObject Shell.Application
$pc = $shell.NameSpace(17)

# Find the Switch MTP device (VID 057e = Nintendo)
$switchItem = $pc.Items() | Where-Object { $_.Name -eq 'Switch' } | Select-Object -First 1
if (-not $switchItem) { throw 'Switch MTP device not found' }
$switchFolder = $switchItem.GetFolder

# Pick the SD card volume (first "SD Card" entry, not "SD Card install")
$sdItem = $switchFolder.Items() | Where-Object { $_.Name -like '*SD Card' -and $_.Name -notlike '*install*' } | Select-Object -First 1
if (-not $sdItem) { throw 'SD Card volume not found' }
$folder = $sdItem.GetFolder

# Walk the sub path segment by segment (empty = the volume root)
$segments = @()
if ($SubPath -ne '') { $segments = $SubPath -split '/' }
foreach ($seg in $segments) {
    $next = $folder.Items() | Where-Object { $_.Name -eq $seg } | Select-Object -First 1
    if (-not $next) { throw ("Path segment not found: " + $seg) }
    $folder = $next.GetFolder
    if (-not $folder) { throw ("Not a folder: " + $seg) }
}

$items = @($folder.Items())
Write-Output ("items=" + $items.Count)

if ($List -or $Newest -le 0) {
    foreach ($it in $items) {
        $date = ''
        try { $date = $folder.GetDetailsOf($it, 3) } catch {}
        '{0}  |  {1}' -f $date, $it.Name
    }
}

if ($Newest -gt 0) {
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
    $logs = @($items | Where-Object { -not $_.IsFolder -and $_.Name -like '*.log' })
    $ordered = @($logs | Sort-Object Name -Descending | Select-Object -First $Newest)
    foreach ($f in $ordered) {
        $dest = Join-Path $OutDir $f.Name
        $destFolder = $shell.NameSpace($OutDir)
        $destFolder.CopyHere($f, 16)  # 16 = yes to all
        $deadline = (Get-Date).AddSeconds(30)
        while (-not (Test-Path $dest) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
        if (Test-Path $dest) {
            '{0} -> {1} ({2} bytes)' -f $f.Name, $dest, (Get-Item $dest).Length
        } else {
            'COPY FAILED: {0}' -f $f.Name
        }
    }
}

if ($Name -ne '') {
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
    $f = $items | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if (-not $f) { throw ("File not found: " + $Name) }
    $destFolder = $shell.NameSpace($OutDir)
    $destFolder.CopyHere($f, 16)
    $dest = Join-Path $OutDir $f.Name
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Test-Path $dest) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if (Test-Path $dest) { '{0} -> {1} ({2} bytes)' -f $f.Name, $dest, (Get-Item $dest).Length }
    else { 'COPY FAILED: {0}' -f $f.Name }
}
