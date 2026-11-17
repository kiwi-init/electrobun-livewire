# Runs each engine's build of the same main process and records launch time
# (process start to the script's first line), idle footprint (the whole
# process tree), size, and the workload's own timings.
param([string]$Dir = (Get-Location).Path, [string]$Bun = (Get-Command bun).Source)
$ErrorActionPreference = "Stop"

function Get-Tree([int]$RootId) {
    $all = @(Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId, Name, WorkingSetSize, PrivatePageCount)
    $ids = New-Object 'System.Collections.Generic.HashSet[int]'
    [void]$ids.Add($RootId)
    do {
        $added = $false
        foreach ($p in $all) {
            if ($ids.Contains([int]$p.ParentProcessId) -and -not $ids.Contains([int]$p.ProcessId)) { [void]$ids.Add([int]$p.ProcessId); $added = $true }
        }
    } while ($added)
    @($all | Where-Object { $ids.Contains([int]$_.ProcessId) })
}

function Invoke-Engine($Exe, [string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $Dir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $boot = $null; $readyMs = $null; $result = $null; $version = $null
    while ($true) {
        $line = $p.StandardOutput.ReadLine()
        if ($null -eq $line) { break }
        if ($line -eq "BOOT" -and $null -eq $boot) { $boot = $sw.Elapsed.TotalMilliseconds }
        elseif ($line.StartsWith("VERSION ")) { $version = $line.Substring(8) }
        elseif ($line.StartsWith("READY ")) { $readyMs = $sw.Elapsed.TotalMilliseconds; $result = $line.Substring(6) | ConvertFrom-Json; break }
    }
    Start-Sleep -Milliseconds 2500
    $tree = Get-Tree $p.Id
    $ws = ($tree | Measure-Object WorkingSetSize -Sum).Sum
    $private = ($tree | Measure-Object PrivatePageCount -Sum).Sum
    & taskkill /T /F /PID $p.Id 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
    [pscustomobject]@{
        boot_ms = $boot; ready_ms = $readyMs; processes = $tree.Count
        working_set_mb = [math]::Round($ws / 1MB, 1); private_mb = [math]::Round($private / 1MB, 1)
        version = $version; result = $result
    }
}

function Median([double[]]$Values) {
    $v = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($v.Count -eq 0) { return $null }
    [math]::Round($v[[int][math]::Floor(($v.Count - 1) / 2)], 2)
}

$webview2Dir = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d' } | Select-Object -First 1
$engines = @(
    @{ name = "Bun (today)"; exe = $Bun; idle = @("bun-idle.js", "$Dir\echo.dll"); work = @("bun-work.js", "$Dir\echo.dll"); ship = (Get-Item $Bun).Length },
    @{ name = "A: hidden WebView2 (V8)"; exe = "$Dir\wv2-host.exe"; idle = @("$Dir\wv2-idle.js"); work = @("$Dir\wv2-work.js"); ship = (Get-Item "$Dir\wv2-host.exe").Length },
    @{ name = "B: QuickJS-ng"; exe = "$Dir\qjs-host.exe"; idle = @("$Dir\qjs-idle.js"); work = @("$Dir\qjs-work.js"); ship = (Get-Item "$Dir\qjs-host.exe").Length }
)

$report = @()
foreach ($e in $engines) {
    Write-Host "== $($e.name)"
    $null = Invoke-Engine $e.exe $e.idle
    $idle = @(1..5 | ForEach-Object { Invoke-Engine $e.exe $e.idle })
    $work = @(1..3 | ForEach-Object { Invoke-Engine $e.exe $e.work })
    $metrics = [ordered]@{}
    foreach ($k in @("json_ms", "loop_ms", "string_ms", "objects_ms", "native_call_us", "async_message_us")) {
        $metrics[$k] = Median ($work | ForEach-Object { $_.result.$k })
    }
    $row = [ordered]@{
        engine = $e.name
        shipped_mb = [math]::Round($e.ship / 1MB, 2)
        launch_ms = Median ($idle | ForEach-Object { $_.boot_ms })
        idle_private_mb = Median ($idle | ForEach-Object { $_.private_mb })
        idle_working_set_mb = Median ($idle | ForEach-Object { $_.working_set_mb })
        processes = $idle[0].processes
        after_work_private_mb = Median ($work | ForEach-Object { $_.private_mb })
        version = ($idle | Where-Object { $_.version } | Select-Object -First 1).version
        check = ($work | ForEach-Object { $_.result.check } | Select-Object -Unique) -join " | "
        metrics = $metrics
    }
    $row | ConvertTo-Json -Depth 4 | Write-Host
    $report += [pscustomobject]$row
}

$md = @()
$md += "### Windows engine spike: the same main process on three engines"
$md += ""
$md += "| | " + (($report | ForEach-Object { $_.engine }) -join " | ") + " |"
$md += "|---|" + (($report | ForEach-Object { "---" }) -join "|") + "|"
$rows = @(
    @("Shipped in the app (MB)", { param($r) $r.shipped_mb }),
    @("Launch to first JS line (ms, median of 5)", { param($r) $r.launch_ms }),
    @("Idle private memory, whole tree (MB)", { param($r) $r.idle_private_mb }),
    @("Idle working set, whole tree (MB)", { param($r) $r.idle_working_set_mb }),
    @("Processes", { param($r) $r.processes }),
    @("Private memory after the workload (MB)", { param($r) $r.after_work_private_mb }),
    @("JSON: 1.4 MB parse x10 (ms)", { param($r) $r.metrics.json_ms }),
    @("Loop: 5M iterations (ms)", { param($r) $r.metrics.loop_ms }),
    @("Strings and regex (ms)", { param($r) $r.metrics.string_ms }),
    @("Map with 200k objects (ms)", { param($r) $r.metrics.objects_ms }),
    @("Native call, synchronous (µs)", { param($r) $r.metrics.native_call_us }),
    @("Message round trip, async (µs)", { param($r) $r.metrics.async_message_us })
)
foreach ($r in $rows) { $md += "| $($r[0]) | " + (($report | ForEach-Object { & $r[1] $_ }) -join " | ") + " |" }
$md += ""
$md += "WebView2 runtime on the runner: $(($report | Where-Object { $_.version } | Select-Object -First 1).version) at $($webview2Dir.FullName) (preinstalled, not shipped)."
$md += "Same results everywhere: $((@($report | ForEach-Object { $_.check }) | Select-Object -Unique).Count -eq 1) ($($report[0].check))"
$md -join "`n" | Set-Content -Encoding utf8 "$Dir\results.md"
$report | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$Dir\results.json"
Get-Content "$Dir\results.md"
