# Measurement runs for the report. Everything here is local: one machine, one docker
# engine, no cloud account involved.
#
#   Experiment 1  throughput and latency percentiles against worker count
#   Experiment 2  scale out reaction time, from issuing the scale to the new replica
#                 being routed to
#
# Output goes to results/bench-*.txt and results/bench-*.csv. Numbers quoted in the report
# come from those files and from nowhere else.

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$WorkerCounts = @(1, 2, 4, 6)
$Duration     = 20
$Warmup       = 5
$Size         = 384
$Iter         = 500
$Runs         = 3     # repetitions of each throughput point
$Repeats      = 5     # repetitions of the reaction time measurement

$bin = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "results") | Out-Null

function Invoke-Quiet($command) {
    cmd /c "$command > NUL 2>&1"
    return $LASTEXITCODE
}

function Get-BackendCount {
    try {
        $body = (Invoke-WebRequest -Uri "http://127.0.0.1:8080/backends" -TimeoutSec 3 -UseBasicParsing).Content
        return [int]($body -split "\s+")[0]
    } catch {
        return -1
    }
}

function Wait-ForBackends([int]$n, [int]$timeoutSeconds = 90) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Get-BackendCount) -eq $n) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

Write-Host "Machine, as reported by the tools themselves:"
Write-Host ("  logical CPUs   " + [Environment]::ProcessorCount)
& g++ --version | Select-Object -First 1 | ForEach-Object { Write-Host "  compiler       $_" }
& cmake --version | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }
& docker --version | ForEach-Object { Write-Host "  $_" }
Write-Host ""

if ((Invoke-Quiet "docker compose up -d --build --scale worker=1") -ne 0) {
    throw "docker compose up failed"
}
if (-not (Wait-ForBackends 1)) { throw "the stack did not come up" }

# --- Experiment 1 -----------------------------------------------------------
$out1 = Join-Path $PSScriptRoot "results\bench-throughput.txt"
"" | Set-Content $out1
$rows = @()

foreach ($n in $WorkerCounts) {
    Write-Host "--- worker=$n ---"
    if ((Invoke-Quiet "docker compose up -d --no-recreate --scale worker=$n worker") -ne 0) {
        throw "scaling to $n failed"
    }
    if (-not (Wait-ForBackends $n)) { throw "the fleet did not reach $n replicas" }
    Start-Sleep -Seconds 3   # let the new replicas finish starting before measuring

    # Closed loop, two outstanding requests per worker thread. Each replica runs a single
    # request thread, so this keeps every worker busy without building an unbounded queue,
    # which is what makes the reported throughput a capacity measurement rather than a
    # measurement of how patient the client was.
    $concurrency = 2 * $n

    # Repeated, because a single run of anything on a desktop is a story, not a
    # measurement. Every repetition is recorded separately; nothing is averaged away
    # before it reaches the file.
    for ($run = 1; $run -le $Runs; $run++) {
        $csv = Join-Path $PSScriptRoot "results\bench-latency-w$n-run$run.csv"
        $report = & (Join-Path $bin "stratus-loadgen.exe") `
            --host 127.0.0.1 --port 8080 `
            --size $Size --iter $Iter `
            --concurrency $concurrency --rate 0 `
            --duration $Duration --warmup $Warmup `
            --csv $csv | Out-String

        Add-Content $out1 "===== workers=$n concurrency=$concurrency run=$run ====="
        Add-Content $out1 $report
        Write-Host $report

        $get = { param($label) ($report -split "`n" | Where-Object { $_ -match "^$label\s" }) -replace "^$label\s+", "" }
        $rows += [pscustomobject]@{
            workers     = $n
            run         = $run
            concurrency = $concurrency
            throughput  = (& $get "throughput")     -replace " req/s", ""
            p50_ms      = (& $get "latency p50")    -replace " ms", ""
            p90_ms      = (& $get "latency p90")    -replace " ms", ""
            p95_ms      = (& $get "latency p95")    -replace " ms", ""
            p99_ms      = (& $get "latency p99")    -replace " ms", ""
            failed      = (& $get "requests failed")
        }
    }
}

$rows | Export-Csv -NoTypeInformation -Path (Join-Path $PSScriptRoot "results\bench-throughput.csv")
Write-Host ""
$rows | Format-Table -AutoSize | Out-String | Write-Host

# --- Experiment 2 -----------------------------------------------------------
Write-Host "--- scale out reaction time, $Repeats repetitions ---"
$out2 = Join-Path $PSScriptRoot "results\bench-reaction.csv"
"repeat,from_replicas,to_replicas,seconds" | Set-Content $out2

if ((Invoke-Quiet "docker compose up -d --no-recreate --scale worker=1 worker") -ne 0) {
    throw "reset to one replica failed"
}
if (-not (Wait-ForBackends 1)) { throw "the fleet did not return to one replica" }

for ($i = 1; $i -le $Repeats; $i++) {
    Start-Sleep -Seconds 2
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Quiet "docker compose up -d --no-recreate --scale worker=2 worker" | Out-Null
    if (-not (Wait-ForBackends 2 60)) { throw "the second replica never appeared" }
    $sw.Stop()
    $s = $sw.Elapsed.TotalSeconds.ToString("0.000", [System.Globalization.CultureInfo]::InvariantCulture)
    Write-Host ("  repeat " + $i + ": " + $s + " s")
    Add-Content $out2 "$i,1,2,$s"

    Invoke-Quiet "docker compose up -d --no-recreate --scale worker=1 worker" | Out-Null
    if (-not (Wait-ForBackends 1 60)) { throw "the fleet did not return to one replica" }
}

Write-Host ""
Write-Host "results\bench-throughput.txt   full load generator reports"
Write-Host "results\bench-throughput.csv   the table above"
Write-Host "results\bench-latency-w*.csv   per request samples for each worker count"
Write-Host "results\bench-reaction.csv     scale out reaction times"
