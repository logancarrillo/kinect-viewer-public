<#
.SYNOPSIS
Installs Kinect Viewer for the current user: no admin, nothing machine-wide.

.DESCRIPTION
Copies a release build (kinect-viewer.exe, kinect-cli.exe and the DLLs beside them), with the
licence and its notices (LICENSE, NOTICE, THIRD_PARTY_NOTICES.md) and each DLL's own licence
texts (licences\), to
%LOCALAPPDATA%\Programs\Kinect Viewer, adds a Start menu shortcut (and a desktop one with
-Desktop), and adds an Apps & Features entry under HKCU so Windows can uninstall it.
Running it again updates the install in place. Close Kinect Viewer first.

Every location can be overridden, which is how the tests run it against a temporary folder
and a temporary HKCU key instead of the real ones.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File windows\packaging\install.ps1 -From build\windows\windows
#>
[CmdletBinding()]
param(
    # The folder holding the release build. Defaults to build.cmd's output for this checkout.
    [string] $From = '',
    [switch] $Desktop,
    [string] $InstallRoot = (Join-Path $env:LOCALAPPDATA 'Programs\Kinect Viewer'),
    [string] $StartMenuDir = [Environment]::GetFolderPath('Programs'),
    [string] $DesktopDir = [Environment]::GetFolderPath('Desktop'),
    [string] $UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KinectViewer'
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$RecordName = 'install-record.json'
$AppName = 'Kinect Viewer'

function Fail([string] $msg, [int] $code) {
    Write-Host "install: $msg"
    exit $code
}

# Defaults that need this script's folder are worked out here, not in param(): Windows
# PowerShell 5.1 leaves $PSScriptRoot empty in parameter defaults.
if (-not $From) { $From = Join-Path $PSScriptRoot '..\..\build\windows\windows' }

# ---- Checks first: nothing is written until they all pass --------------------------------
if (-not $UninstallKey.StartsWith('HKCU:\', [StringComparison]::OrdinalIgnoreCase)) {
    Fail "refused: the uninstall entry must be under HKCU (per-user), not '$UninstallKey'" 3
}
$root = [IO.Path]::GetFullPath($InstallRoot)
$machineDirs = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432, $env:ProgramData, $env:windir) |
    Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') + '\' }
foreach ($m in $machineDirs) {
    if (($root.TrimEnd('\') + '\').StartsWith($m, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "refused: per-user install only, not under $m" 3
    }
}
if ((Split-Path -Leaf $root) -ne $AppName) {
    Fail "refused: the install folder must be named '$AppName' (got '$root')" 3
}
$src = [IO.Path]::GetFullPath($From)
$exe = Join-Path $src 'kinect-viewer.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Fail "no kinect-viewer.exe in '$src'. Build it (windows\scripts\build.cmd) or pass -From <folder>." 2
}
$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if (-not $version) { $version = '' }

# What gets copied: the two programs (the CLI if it's there) and the runtime DLLs beside them.
$files = @('kinect-viewer.exe')
if (Test-Path -LiteralPath (Join-Path $src 'kinect-cli.exe') -PathType Leaf) { $files += 'kinect-cli.exe' }
$files += @(Get-ChildItem -LiteralPath $src -Filter '*.dll' -File | ForEach-Object { $_.Name } | Sort-Object)
# The licence and its notices go beside them, from this checkout: whoever copies the install folder
# copies them too (THIRD_PARTY_NOTICES.md). Each one the checkout has; recorded, and removed, as the rest.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$notices = @(@('LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md') |
    Where-Object { Test-Path -LiteralPath (Join-Path $repo $_) -PathType Leaf })
# And each bundled DLL's own licence texts, named after it (licences\<dll name>.*, licences\README.md).
$licenceDir = Join-Path $PSScriptRoot 'licences'
$dllLicences = @(foreach ($dll in @($files | Where-Object { $_ -like '*.dll' })) {
        $base = [IO.Path]::GetFileNameWithoutExtension($dll)
        if (Test-Path -LiteralPath $licenceDir -PathType Container) {
            Get-ChildItem -LiteralPath $licenceDir -File -Filter "$base.*" | ForEach-Object { $_.Name } | Sort-Object
        }
    })
$recorded = @($files + $notices + $dllLicences)

# ---- Keep-running watchdogs of this install stand down first -----------------------------------
# A watchdog (kinect-viewer.exe --keep-running) restarts a viewer that ends without a clean stop.
# One running from the folder being replaced is told to stand down, and waited for, before
# anything is copied: a viewer killed for the install must not be started again under the copy,
# and the watchdog's own exe must be free to replace. Only this install folder's: a test's
# temporary folder has none, and another copy elsewhere is left alone.
function Stop-KeepRunning([string] $folder) {
    $prefix = $folder.TrimEnd('\') + '\'
    # The viewer itself still open from this folder: refused before anything is touched, so its
    # watchdog is not stood down for an install that can't copy over a running exe.
    $open = @(Get-CimInstance Win32_Process -Filter "Name = 'kinect-viewer.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and
                       $_.CommandLine -notmatch '--keep-running' })
    if ($open.Count) { Fail "Kinect Viewer is open (pid $(($open | ForEach-Object { $_.ProcessId }) -join ', ')). Close Kinect Viewer first, then run this again." 4 }
    $found = @(Get-CimInstance Win32_Process -Filter "Name = 'kinect-viewer.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and
                       $_.CommandLine -match '--keep-running' })
    foreach ($w in $found) {
        try {
            $ev = [System.Threading.EventWaitHandle]::OpenExisting("Local\KinectViewer-KeepRunning-StandDown-$($w.ProcessId)")
            [void] $ev.Set()
            $ev.Dispose()
        } catch { Write-Host "install: the keep-running watchdog (pid $($w.ProcessId)) could not be told to stand down" }
    }
    foreach ($w in $found) {
        $p = Get-Process -Id $w.ProcessId -ErrorAction SilentlyContinue
        if ($p -and -not $p.WaitForExit(10000)) { Fail "the keep-running watchdog (pid $($w.ProcessId)) did not stand down; close Kinect Viewer and try again" 4 }
    }
    if ($found.Count) { Write-Host "install: the keep-running watchdog stood down ($($found.Count))" }
}
Stop-KeepRunning $root

# ---- Copy -----------------------------------------------------------------------------------
$old = $null
$recordPath = Join-Path $root $RecordName
if (Test-Path -LiteralPath $recordPath -PathType Leaf) {
    $old = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
}
New-Item -ItemType Directory -Force -Path $root | Out-Null
try {
    foreach ($f in $files) { Copy-Item -LiteralPath (Join-Path $src $f) -Destination (Join-Path $root $f) -Force }
    foreach ($n in $notices) { Copy-Item -LiteralPath (Join-Path $repo $n) -Destination (Join-Path $root $n) -Force }
    foreach ($l in $dllLicences) { Copy-Item -LiteralPath (Join-Path $licenceDir $l) -Destination (Join-Path $root $l) -Force }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination (Join-Path $root 'uninstall.ps1') -Force
} catch {
    Fail "couldn't copy into '$root': $($_.Exception.Message). If Kinect Viewer is open, close it and run this again." 1
}
# Files an older install had that this build doesn't: removed (only files that install listed).
if ($old -and $old.files) {
    foreach ($f in $old.files) {
        if ($recorded -notcontains $f -and $f -notmatch '[\\/]') {
            Remove-Item -LiteralPath (Join-Path $root $f) -Force -ErrorAction SilentlyContinue
        }
    }
}
$installed = Join-Path $root 'kinect-viewer.exe'

# ---- Shortcuts ------------------------------------------------------------------------------
$shell = New-Object -ComObject WScript.Shell
function New-AppShortcut([string] $dir) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir "$AppName.lnk"
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = $installed
    $lnk.WorkingDirectory = $root
    $lnk.IconLocation = "$installed,0"
    $lnk.Description = 'Live depth and colour from the Kinect v2 sensor'
    $lnk.Save()
    return $path
}
$shortcuts = @(New-AppShortcut $StartMenuDir)
$desktopLink = Join-Path $DesktopDir "$AppName.lnk"
if ($Desktop) {
    $shortcuts += New-AppShortcut $DesktopDir
} elseif ($old -and $old.shortcuts -and ($old.shortcuts -contains $desktopLink)) {
    $shortcuts += $desktopLink # an earlier -Desktop install's shortcut stays, and stays recorded
}

# ---- Apps & Features (HKCU) -----------------------------------------------------------------
$sizeKb = [int][Math]::Ceiling((($recorded | ForEach-Object { (Get-Item -LiteralPath (Join-Path $root $_)).Length } |
    Measure-Object -Sum).Sum) / 1KB)
$uninstallCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $root 'uninstall.ps1')`""
New-Item -Path $UninstallKey -Force | Out-Null
$values = [ordered]@{
    DisplayName     = $AppName
    DisplayVersion  = $version
    Publisher       = 'The Kinect Viewer Authors' # as NOTICE; nothing looks an install up by it
    DisplayIcon     = "$installed,0"
    InstallLocation = $root
    InstallDate     = (Get-Date -Format 'yyyyMMdd')
    UninstallString = $uninstallCmd
    QuietUninstallString = $uninstallCmd
}
foreach ($k in $values.Keys) { New-ItemProperty -Path $UninstallKey -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
foreach ($k in 'NoModify', 'NoRepair') { New-ItemProperty -Path $UninstallKey -Name $k -Value 1 -PropertyType DWord -Force | Out-Null }
New-ItemProperty -Path $UninstallKey -Name 'EstimatedSize' -Value $sizeKb -PropertyType DWord -Force | Out-Null

# ---- The record uninstall.ps1 follows: it removes exactly these and nothing else ------------
[ordered]@{
    app          = $AppName
    version      = $version
    files        = $recorded
    shortcuts    = $shortcuts
    uninstallKey = $UninstallKey
} | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding UTF8

Write-Host "install: Kinect Viewer $version installed to $root"
Write-Host "install: Start menu shortcut: $($shortcuts[0])"
if ($shortcuts.Count -gt 1) { Write-Host "install: desktop shortcut: $($shortcuts[1])" }
exit 0
