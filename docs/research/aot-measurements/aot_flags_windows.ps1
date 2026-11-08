# aot_flags_windows.ps1 -- compile cost of winapk-aot's C with Win.apk's own clang, per clang setting.
#
# Section 8, step 1 of docs/research/aot-measurements.md. Uses what an installed Win.apk has:
# bin\winapk-aot.exe, clang\bin\clang.exe, and the arm64 libraries of installed apps. For up to
# four libraries it writes the C (parts of about 20,000 instructions), takes the first parts of each,
# and compiles the same files with each setting, one clang at a time, recording CPU time, peak memory
# and object size. Nothing of Win.apk is changed; the work folder is in %TEMP% and is deleted at the
# end. Results: on screen, and winapk-aot-flags.csv on the desktop.
#
#   powershell -ExecutionPolicy Bypass -File aot_flags_windows.ps1 [-PartsPerLib 4]

param([int]$PartsPerLib = 4)
$ErrorActionPreference = 'Stop'

$app = Join-Path $env:LOCALAPPDATA 'Programs\Win.apk'
$aot = Join-Path $app 'bin\winapk-aot.exe'
$clang = Join-Path $app 'clang\bin\clang.exe'
foreach ($f in @($aot, $clang)) {
    if (-not (Test-Path $f)) { Write-Host "Not found: $f (is Win.apk installed for this user?)"; exit 1 }
}
if (Get-Process clang -ErrorAction SilentlyContinue) {
    Write-Host 'clang.exe is running (Win.apk is translating an app in the background).'
    Write-Host 'Close the apps, wait until no clang.exe is left in Task Manager, then run this again.'
    exit 1
}

$settings = @('-O1', '-O2', '-O2 -mllvm -fast-isel')
$base = '-march=native -fno-strict-aliasing -w -c'

$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1)
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$clangVersion = (& $clang --version 2>$null | Select-Object -First 1)
Write-Host "CPU: $($cpu.Name), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical; RAM $ram GB"
Write-Host "clang: $clangVersion"

$apps = Join-Path $env:LOCALAPPDATA 'Win.apk\android\data\app'
$libs = @(Get-ChildItem $apps -Recurse -Filter '*.so' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\lib\\arm64\\' -and $_.Length -gt 1MB -and $_.Length -lt 60MB } |
    Sort-Object Length -Descending | Group-Object Name | ForEach-Object { $_.Group[0] } | Select-Object -First 4)
if ($libs.Count -eq 0) {
    Write-Host 'No arm64 library found: install an app that has only arm64 native code (Waze, for example) first.'
    exit 1
}

$work = Join-Path $env:TEMP 'winapk-aot-flags'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null

# One compile: CPU seconds, peak working set (MB), object bytes.
function Measure-Compile([string]$flags, [string]$src, [string]$obj) {
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo.FileName = $clang
    $p.StartInfo.Arguments = "$base $flags -o `"$obj`" `"$src`""
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.CreateNoWindow = $true
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$p.Start()
    $peak = 0; $cpuS = 0
    while (-not $p.HasExited) {
        try {
            $p.Refresh()
            if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 }
            $cpuS = $p.TotalProcessorTime.TotalSeconds
        } catch { }
        Start-Sleep -Milliseconds 100
    }
    $p.WaitForExit()
    try { $cpuS = $p.TotalProcessorTime.TotalSeconds } catch { }
    $size = 0
    if (Test-Path $obj) { $size = (Get-Item $obj).Length; Remove-Item $obj }
    [pscustomobject]@{ Cpu = $cpuS; Wall = $watch.Elapsed.TotalSeconds; PeakMB = [math]::Round($peak / 1MB); Bytes = $size; Code = $p.ExitCode }
}

$rows = @()
$n = 0
foreach ($lib in $libs) {
    $n++
    $dir = Join-Path $work "lib$n"
    New-Item -ItemType Directory $dir | Out-Null
    $c = Join-Path $dir 'x.c'
    # winapk-aot reports on stderr; Windows PowerShell turns that into errors, which must not stop the script.
    $ErrorActionPreference = 'Continue'
    $out = & $aot $lib.FullName $c 100000 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 1
    $ErrorActionPreference = 'Stop'
    $insns = 0
    if ("$out" -match '(\d+) functions, (\d+) instructions') { $insns = [int64]$Matches[2] }
    $parts = @(Get-ChildItem $dir -Filter 'x.c.*.c' | Sort-Object { [int]($_.Name -replace '^x\.c\.(\d+)\.c$', '$1') } |
        Select-Object -First $PartsPerLib)
    if ($parts.Count -eq 0) { Write-Host "$($lib.Name): no parts (too small?)"; continue }
    $allParts = @(Get-ChildItem $dir -Filter 'x.c.*.c').Count
    $partInsns = if ($allParts -gt 0) { [int64]($insns / $allParts) } else { 0 }
    Write-Host ""
    Write-Host "$($lib.Name): $insns instructions; $($parts.Count) of $allParts parts (~$partInsns instructions each)"
    foreach ($s in $settings) {
        foreach ($part in $parts) {
            $r = Measure-Compile $s $part.FullName ($part.FullName + '.o')
            $rows += [pscustomobject]@{
                Library = $lib.Name; Setting = $s; Part = $part.Name; Instructions = $partInsns
                CpuS = [math]::Round($r.Cpu, 1); WallS = [math]::Round($r.Wall, 1); PeakMB = $r.PeakMB
                ObjectBytes = $r.Bytes; ExitCode = $r.Code
            }
            Write-Host ("  {0,-22} {1,-10} {2,6:N1} s CPU {3,6:N1} s {4,6} MB peak {5,10:N0} bytes{6}" -f $s, $part.Name,
                $r.Cpu, $r.Wall, $r.PeakMB, $r.Bytes, $(if ($r.Code -ne 0) { "  FAILED ($($r.Code))" } else { '' }))
        }
    }
}

Write-Host ""
Write-Host "Summary (same files for every setting):"
foreach ($s in $settings) {
    $g = @($rows | Where-Object { $_.Setting -eq $s -and $_.ExitCode -eq 0 })
    $cpuTotal = ($g | Measure-Object CpuS -Sum).Sum
    $wallTotal = ($g | Measure-Object WallS -Sum).Sum
    $peakMax = ($g | Measure-Object PeakMB -Maximum).Maximum
    $bytes = ($g | Measure-Object ObjectBytes -Sum).Sum
    $failed = @($rows | Where-Object { $_.Setting -eq $s -and $_.ExitCode -ne 0 }).Count
    Write-Host ("  {0,-22} CPU {1,7:N1} s   time {2,7:N1} s   largest peak {3,5} MB   objects {4,6:N1} MB   failed {5}" -f $s,
        $cpuTotal, $wallTotal, $peakMax, ($bytes / 1MB), $failed)
}
$csv = Join-Path ([Environment]::GetFolderPath('Desktop')) 'winapk-aot-flags.csv'
"# CPU: $($cpu.Name), $($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) logical; RAM $ram GB; clang: $clangVersion" |
    Out-File $csv -Encoding utf8
$rows | ConvertTo-Csv -NoTypeInformation | Out-File $csv -Append -Encoding utf8
Remove-Item $work -Recurse -Force
Write-Host ""
Write-Host "Saved: $csv"
