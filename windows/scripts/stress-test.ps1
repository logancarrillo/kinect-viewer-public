# Runs one test of a kv_test binary many times in parallel copies, to shake out crashes that need a busy PC.
#
# How to run it (after a normal build, windows\scripts\build.cmd):
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows\scripts\stress-test.ps1 `
#       -Exe build\windows\windows\test_stream_e2e.exe -Filter v2_unpairing_goes_through_the_disconnect_hook -Copies 6 -Runs 60
#
# What it runs: only the test executables a build makes (test_*.exe under a build folder), with one test name as the
# filter. Those are synthetic: the synthetic engine, loopback sockets and their own temp state folders. It never
# runs the viewer or the CLI, so it never touches %USERPROFILE%\kinect-viewer\.host-state, a real recordings or
# camera-roll folder, or the real Kinect; anything else given as -Exe is refused.
#
# What it reports: a copy stops at its first crash (an exit code other than 0, 1 or 77), and the script then exits 1.
# An ordinary check failure (exit 1) is counted and reported, not treated as a crash. So is a run whose timing floors
# were skipped on a busy PC (exit 77, kv_load.hpp), which parallel copies make likely. Logs go to a new temp folder.
param(
    [Parameter(Mandatory)][string] $Exe,
    [Parameter(Mandatory)][string] $Filter,
    [int] $Copies = 6,
    [int] $Runs = 60,
    [string] $OutDir = (Join-Path ([IO.Path]::GetTempPath()) ("kv-stress-" + [Guid]::NewGuid().ToString('N').Substring(0, 8)))
)
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$leaf = Split-Path -Leaf $Exe
if ($leaf -notlike 'test_*.exe' -or ($Exe -notmatch '\\build\\')) {
    Write-Error "refused: $Exe isn't a test executable from a build folder (test_*.exe under ...\build\...)"
    exit 2
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$jobs = @()
for ($k = 1; $k -le $Copies; $k++) {
    $log = Join-Path $OutDir "copy-$k.log"
    $jobs += Start-Job -ArgumentList $Exe, $Filter, $Runs, $log -ScriptBlock {
        param($Exe, $Filter, $Runs, $log)
        $plain = 0
        $skipped = 0
        for ($i = 1; $i -le $Runs; $i++) {
            $out = & $Exe $Filter 2>&1 | Out-String
            Add-Content -LiteralPath $log -Value $out
            $rc = $LASTEXITCODE
            if ($rc -eq 0) { continue }
            if ($rc -eq 1) { $plain++; continue }
            if ($rc -eq 77) { $skipped++; continue }
            return [pscustomobject]@{ Crashed = $true; Run = $i; Code = ('0x{0:X8}' -f $rc); Plain = $plain; Skipped = $skipped }
        }
        return [pscustomobject]@{ Crashed = $false; Run = $Runs; Code = ''; Plain = $plain; Skipped = $skipped }
    }
}
$results = @($jobs | Wait-Job | Receive-Job)
$jobs | Remove-Job
$crashes = 0
$runs = 0
$plain = 0
$skipped = 0
for ($k = 0; $k -lt $results.Count; $k++) {
    $r = $results[$k]
    $runs += $r.Run
    $plain += $r.Plain
    $skipped += $r.Skipped
    if ($r.Crashed) {
        $crashes++
        "copy $($k + 1): CRASH at run $($r.Run), exit code $($r.Code) (ordinary check failures before it: $($r.Plain))"
    } else {
        "copy $($k + 1): $($r.Run) runs, no crash (ordinary check failures: $($r.Plain))"
    }
}
"total: $runs runs in $Copies copies, $crashes crash(es), $plain ordinary check failure(s), $skipped run(s) with timing floors skipped on a busy PC; logs in $OutDir"
if ($crashes -gt 0) { exit 1 }
exit 0
