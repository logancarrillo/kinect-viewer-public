<#
.SYNOPSIS
Uninstalls Kinect Viewer for the current user.

.DESCRIPTION
Follows install-record.json in the install folder and removes exactly what install.ps1 made:
the copied files, its shortcuts and its Apps & Features entry. That entry is deleted only if it
sits directly under HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\; any other
recorded key is left alone, with a warning. Anything else in the folder
is left alone (and so is the folder, if anything is left in it). Recordings and settings live
elsewhere and are not touched. Close Kinect Viewer first.

Apps & Features runs the copy of this script inside the install folder.
#>
[CmdletBinding()]
param(
    # Default: the folder this copy runs from (as Apps & Features runs it), else the usual place.
    [string] $InstallRoot = ''
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
# Only entries under this key are ever deleted; any other recorded key is left alone.
$UninstallParent = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\'

function Fail([string] $msg, [int] $code) {
    Write-Host "uninstall: $msg"
    exit $code
}

# Defaults are worked out here, not in param(): Windows PowerShell 5.1 leaves $PSScriptRoot empty
# in parameter defaults.
if (-not $InstallRoot) {
    if ($PSScriptRoot -and (Split-Path -Leaf $PSScriptRoot) -eq 'Kinect Viewer') { $InstallRoot = $PSScriptRoot }
    else { $InstallRoot = Join-Path $env:LOCALAPPDATA 'Programs\Kinect Viewer' }
}
$root = [IO.Path]::GetFullPath($InstallRoot)
$recordPath = Join-Path $root 'install-record.json'
if (-not (Test-Path -LiteralPath $recordPath -PathType Leaf)) {
    Fail "nothing to uninstall: no install record in '$root'" 2
}
$rec = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
if ($rec.app -ne 'Kinect Viewer') { Fail "refused: '$recordPath' isn't a Kinect Viewer install record" 3 }
$key = "$($rec.uninstallKey)"
$tail = if ($key.StartsWith($UninstallParent, [StringComparison]::OrdinalIgnoreCase)) { $key.Substring($UninstallParent.Length) } else { '' }
$keyIsOurs = $tail -and $tail -notmatch '[\\/]' -and $tail -ne '..'

# Leave the folder before removing it (Apps & Features may start us inside it).
Set-Location -LiteralPath ([IO.Path]::GetTempPath())

# The viewer still open from this folder: refused before anything is touched. Then any keep-running watchdog from
# this folder (kinect-viewer.exe --keep-running) is told to stand down and waited for, so nothing restarts a viewer
# while its files go. Only this folder's: a test's temporary folder has none.
$prefix = $root.TrimEnd('\') + '\'
$ours = @(Get-CimInstance Win32_Process -Filter "Name = 'kinect-viewer.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
$open = @($ours | Where-Object { $_.CommandLine -notmatch '--keep-running' })
if ($open.Count) { Fail "Kinect Viewer is open (pid $(($open | ForEach-Object { $_.ProcessId }) -join ', ')). Close Kinect Viewer first, then run this again." 4 }
foreach ($w in @($ours | Where-Object { $_.CommandLine -match '--keep-running' })) {
    try {
        $ev = [System.Threading.EventWaitHandle]::OpenExisting("Local\KinectViewer-KeepRunning-StandDown-$($w.ProcessId)")
        [void] $ev.Set()
        $ev.Dispose()
    } catch { Write-Host "uninstall: the keep-running watchdog (pid $($w.ProcessId)) could not be told to stand down" }
    $p = Get-Process -Id $w.ProcessId -ErrorAction SilentlyContinue
    if ($p -and -not $p.WaitForExit(10000)) { Fail "the keep-running watchdog (pid $($w.ProcessId)) did not stand down; close Kinect Viewer and try again" 4 }
}

# Files first: a running copy keeps its exe locked, so stop before touching anything else.
foreach ($f in @($rec.files)) {
    if ($f -match '[\\/]' -or $f -eq '..') { continue } # only plain names in the install folder
    $p = Join-Path $root $f
    try {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    } catch {
        Fail "couldn't remove '$p': $($_.Exception.Message). If Kinect Viewer is open, close it and run this again." 1
    }
}
foreach ($s in @($rec.shortcuts)) {
    if ("$s".EndsWith('\Kinect Viewer.lnk', [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $s)) {
        Remove-Item -LiteralPath $s -Force
    }
}
if ($keyIsOurs) {
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
} else {
    Write-Host "uninstall: warning: the recorded entry '$key' isn't directly under $UninstallParent, so it was left alone"
}

foreach ($f in 'uninstall.ps1', 'install-record.json') {
    $p = Join-Path $root $f
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
}
if (-not (Get-ChildItem -LiteralPath $root -Force | Select-Object -First 1)) {
    Remove-Item -LiteralPath $root -Force
    Write-Host "uninstall: Kinect Viewer removed"
} else {
    Write-Host "uninstall: Kinect Viewer removed; other files were left in $root"
}
exit 0
