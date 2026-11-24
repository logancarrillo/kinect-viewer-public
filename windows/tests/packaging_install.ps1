# ctest packaging_install_uninstall: runs windows/packaging/install.ps1 and uninstall.ps1 against a
# temporary folder and a temporary HKCU key only (never the real Start menu, desktop, Programs folder
# or Apps & Features), and checks what they make and remove. The uninstall is run the way Apps &
# Features runs it: the installed copy, through the recorded UninstallString, with no arguments.
# uninstall.ps1 deletes a recorded key only under HKCU's real Uninstall key, so it leaves the test's
# temporary key (with a warning) and the finally block removes it.
#   powershell -NoProfile -ExecutionPolicy Bypass -File packaging_install.ps1 -From <folder with kinect-viewer.exe>
param([Parameter(Mandatory)][string] $From)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$pkg = Join-Path $PSScriptRoot '..\packaging'
$failures = 0
function Check([bool] $cond, [string] $what) {
    if ($cond) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what"; $script:failures++ }
}

$id = [Guid]::NewGuid().ToString('N')
$tmp = Join-Path ([IO.Path]::GetTempPath()) "kv-packaging-$id"
$root = Join-Path $tmp 'LocalAppData\Programs\Kinect Viewer'
$startMenu = Join-Path $tmp 'StartMenu\Programs'
$desktop = Join-Path $tmp 'Desktop'
$testParent = "HKCU:\Software\KinectViewerPackagingTest-$id"
$key = "$testParent\Uninstall\KinectViewer"
$common = @{ InstallRoot = $root; StartMenuDir = $startMenu; DesktopDir = $desktop; UninstallKey = $key }
$ps = (Get-Process -Id $PID).Path

# Runs a program and echoes its output. Its stderr is just output here: with 'Stop', Windows
# PowerShell would turn the first stderr line into an exception and end the test mid-run.
function Native([string] $exe, [string[]] $argv) {
    $ErrorActionPreference = 'Continue'
    $out = & $exe @argv 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "    | $_" }
    $script:lastOut = ($out | Out-String)
    return $code
}

function Run([string] $script, [hashtable] $named, [switch] $DesktopSwitch) {
    $argv = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $pkg $script))
    foreach ($k in $named.Keys) { $argv += "-$k"; $argv += $named[$k] }
    if ($DesktopSwitch) { $argv += '-Desktop' }
    return (Native $ps $argv)
}

function Target([string] $lnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($lnk) }

try {
    $exe = Join-Path $From 'kinect-viewer.exe'
    Check (Test-Path -LiteralPath $exe) "the build has kinect-viewer.exe ($From)"
    $dlls = @(Get-ChildItem -LiteralPath $From -Filter '*.dll' -File | ForEach-Object { $_.Name })

    Write-Host '-- refusals write nothing'
    $code = Run 'install.ps1' @{ From = $From; InstallRoot = (Join-Path $env:ProgramFiles 'Kinect Viewer'); StartMenuDir = $startMenu; DesktopDir = $desktop; UninstallKey = $key }
    Check ($code -eq 3) "an install under Program Files is refused (exit $code)"
    $code = Run 'install.ps1' @{ From = $From; InstallRoot = $root; StartMenuDir = $startMenu; DesktopDir = $desktop; UninstallKey = 'HKLM:\Software\KinectViewerPackagingTest' }
    Check ($code -eq 3) "an HKLM uninstall entry is refused (exit $code)"
    $code = Run 'install.ps1' @{ From = (Join-Path $tmp 'nothing-here'); InstallRoot = $root; StartMenuDir = $startMenu; DesktopDir = $desktop; UninstallKey = $key }
    Check ($code -eq 2) "a folder without kinect-viewer.exe is refused (exit $code)"
    Check (-not (Test-Path -LiteralPath $tmp)) 'nothing was written by the refusals'
    Check (-not (Test-Path -LiteralPath $testParent)) 'no registry key was made by the refusals'

    Write-Host '-- install (Start menu only)'
    $code = Run 'install.ps1' ($common + @{ From = $From })
    Check ($code -eq 0) "install exits 0 (exit $code)"
    $installed = Join-Path $root 'kinect-viewer.exe'
    Check (Test-Path -LiteralPath $installed) 'kinect-viewer.exe is in the install folder'
    foreach ($d in $dlls) { Check (Test-Path -LiteralPath (Join-Path $root $d)) "$d is in the install folder" }
    Check (Test-Path -LiteralPath (Join-Path $root 'uninstall.ps1')) 'uninstall.ps1 is in the install folder'
    # the licence and its notices beside the programs, the checkout's own, and in the record
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $pkg '..\..'))
    $listed = @((Get-Content -LiteralPath (Join-Path $root 'install-record.json') -Raw | ConvertFrom-Json).files)
    foreach ($n in 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md') {
        $copy = Join-Path $root $n
        Check ((Test-Path -LiteralPath $copy) -and [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($copy), [IO.File]::ReadAllBytes((Join-Path $repoRoot $n)))) "$n is in the install folder, as the checkout has it"
        Check ($listed -contains $n) "$n is in the install record, so the uninstall removes it"
    }
    # each bundled DLL's own licence texts beside it (windows\packaging\licences\<dll name>.*), the
    # checkout's own, and in the record: every DLL the build has carries at least one
    $licences = Join-Path $pkg 'licences'
    foreach ($d in $dlls) {
        $base = [IO.Path]::GetFileNameWithoutExtension($d)
        $texts = @(Get-ChildItem -LiteralPath $licences -File -Filter "$base.*" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        Check ($texts.Count -gt 0) "$d has its licence texts in windows\packaging\licences"
        foreach ($t in $texts) {
            $copy = Join-Path $root $t
            Check ((Test-Path -LiteralPath $copy) -and [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($copy), [IO.File]::ReadAllBytes((Join-Path $licences $t)))) "$t is in the install folder beside $d"
            Check ($listed -contains $t) "$t is in the install record, so the uninstall removes it"
        }
    }
    # (Bytes compared with .NET: run from pwsh 7, Windows PowerShell can't load Get-FileHash's module.)
    $same = [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($installed), [IO.File]::ReadAllBytes($exe))
    Check $same 'the installed exe is the build''s'
    $lnk = Join-Path $startMenu 'Kinect Viewer.lnk'
    Check (Test-Path -LiteralPath $lnk) 'the Start menu shortcut exists'
    if (Test-Path -LiteralPath $lnk) {
        $t = Target $lnk
        Check ($t.TargetPath -eq $installed) "it opens the installed exe ($($t.TargetPath))"
        Check ($t.IconLocation -eq "$installed,0") "its icon is the exe's ($($t.IconLocation))"
    }
    Check (-not (Test-Path -LiteralPath (Join-Path $desktop 'Kinect Viewer.lnk'))) 'no desktop shortcut without -Desktop'
    Check (Test-Path -LiteralPath $key) 'the HKCU uninstall entry exists'
    if (Test-Path -LiteralPath $key) {
        $p = Get-ItemProperty -LiteralPath $key
        $ver = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
        Check ($p.DisplayName -eq 'Kinect Viewer') 'DisplayName is Kinect Viewer'
        Check ($p.DisplayVersion -eq $ver -and $ver) "DisplayVersion is the exe's ($($p.DisplayVersion))"
        Check ($p.Publisher -eq 'The Kinect Viewer Authors') 'Publisher is The Kinect Viewer Authors (as NOTICE)'
        Check ($p.DisplayIcon -eq "$installed,0") 'DisplayIcon is the installed exe'
        Check ($p.UninstallString -like "*$(Join-Path $root 'uninstall.ps1')*") 'UninstallString runs the installed uninstall.ps1'
        Check ($p.NoModify -eq 1 -and $p.NoRepair -eq 1 -and $p.EstimatedSize -gt 0) 'NoModify, NoRepair and EstimatedSize are set'
    }

    Write-Host '-- install again with -Desktop (an update in place), over an entry an earlier version wrote'
    # Earlier versions wrote another Publisher. Nothing finds an install by it (the record and the key's path do),
    # so an update over that entry goes ahead and writes today's.
    if (Test-Path -LiteralPath $key) { New-ItemProperty -Path $key -Name 'Publisher' -Value 'An earlier publisher' -PropertyType String -Force | Out-Null }
    $code = Run 'install.ps1' ($common + @{ From = $From }) -DesktopSwitch
    Check ($code -eq 0) "the second install exits 0 (exit $code)"
    Check ((Get-ItemProperty -LiteralPath $key).Publisher -eq 'The Kinect Viewer Authors') 'the update writes today''s Publisher over the earlier one'
    $installText = Get-Content -LiteralPath (Join-Path $pkg 'install.ps1') -Raw
    $uninstallText = Get-Content -LiteralPath (Join-Path $pkg 'uninstall.ps1') -Raw
    Check (([regex]::Matches($installText, 'Publisher|CompanyName')).Count -eq 1) 'install.ps1 names Publisher once, where it writes it'
    Check (-not ($uninstallText -match 'Publisher|CompanyName')) 'uninstall.ps1 never looks at Publisher or CompanyName'
    $dlnk = Join-Path $desktop 'Kinect Viewer.lnk'
    Check (Test-Path -LiteralPath $dlnk) 'the desktop shortcut exists'
    if (Test-Path -LiteralPath $dlnk) { Check ((Target $dlnk).TargetPath -eq $installed) 'it opens the installed exe' }

    Write-Host '-- a file someone else put in the folder survives the uninstall'
    $stray = Join-Path $root 'my-notes.txt'
    Set-Content -LiteralPath $stray -Value 'not the installer''s'

    Write-Host '-- uninstall the way Apps & Features does: the recorded UninstallString, no arguments'
    $cmdline = (Get-ItemProperty -LiteralPath $key).UninstallString
    Check ($cmdline -like 'powershell.exe *') "it runs under Windows PowerShell ($cmdline)"
    # Started with the command line exactly as recorded (no PowerShell argument quoting in between).
    $psi = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/d /c $cmdline"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEndAsync(); $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $script:lastOut = $stdout.Result + $stderr
    $script:lastOut -split "`r?`n" | Where-Object { $_ } | ForEach-Object { Write-Host "    | $_" }
    $code = $proc.ExitCode
    Check ($code -eq 0) "uninstall exits 0 (exit $code)"
    Check ($script:lastOut -match 'left alone') 'it warns that the key outside the Uninstall key is left alone'
    Check (-not (Test-Path -LiteralPath $installed)) 'kinect-viewer.exe is gone'
    foreach ($d in $dlls) { Check (-not (Test-Path -LiteralPath (Join-Path $root $d))) "$d is gone" }
    Check (-not (Test-Path -LiteralPath $lnk)) 'the Start menu shortcut is gone'
    Check (-not (Test-Path -LiteralPath $dlnk)) 'the desktop shortcut is gone'
    Check (Test-Path -LiteralPath $key) 'the temporary key (outside the Uninstall key) is left'
    Check (Test-Path -LiteralPath $stray) 'the other file is still there'
    Remove-Item -LiteralPath $stray -Force

    Write-Host '-- install and uninstall once more: the folder itself goes when nothing else is in it'
    $code = Run 'install.ps1' ($common + @{ From = $From })
    Check ($code -eq 0) "install exits 0 (exit $code)"
    $code = Run 'uninstall.ps1' @{ InstallRoot = $root }
    Check ($code -eq 0) "uninstall exits 0 (exit $code)"
    Check (-not (Test-Path -LiteralPath $root)) 'the install folder is gone'
    $code = Run 'uninstall.ps1' @{ InstallRoot = $root }
    Check ($code -eq 2) "a second uninstall says there's nothing to do (exit $code)"

    Write-Host '-- a record naming a key outside the Uninstall key: that key is never deleted'
    $precious = "$testParent\Precious"
    New-Item -Path $precious -Force | Out-Null
    New-ItemProperty -Path $precious -Name 'Keep' -Value 'me' -PropertyType String -Force | Out-Null
    $code = Run 'install.ps1' ($common + @{ From = $From })
    Check ($code -eq 0) "install exits 0 (exit $code)"
    $recPath = Join-Path $root 'install-record.json'
    $rec = Get-Content -LiteralPath $recPath -Raw | ConvertFrom-Json
    $rec.uninstallKey = $precious
    $rec | ConvertTo-Json | Set-Content -LiteralPath $recPath -Encoding UTF8
    $code = Run 'uninstall.ps1' @{ InstallRoot = $root }
    Check ($code -eq 0) "uninstall exits 0 (exit $code)"
    Check (-not (Test-Path -LiteralPath (Join-Path $root 'kinect-viewer.exe'))) 'the files are still removed'
    Check ((Test-Path -LiteralPath $precious) -and ((Get-ItemProperty -LiteralPath $precious).Keep -eq 'me')) 'the outside key and its value are untouched'
} finally {
    if (Test-Path -LiteralPath $testParent) { Remove-Item -LiteralPath $testParent -Recurse -Force }
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
}

Write-Host "$failures failure(s)"
if ($failures -gt 0) { exit 1 }
exit 0
