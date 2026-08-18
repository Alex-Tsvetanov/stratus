# Stratus end to end demonstration.
#
#   1. bring the stack up with a single worker
#   2. start the autoscaling controller on the host
#   3. drive open loop load at a rate one worker cannot absorb
#   4. stop the load and let the controller give the capacity back
#   5. print the decision timeline, the load report and the cost model
#
# Everything measured here is local. Nothing is deployed to any cloud account.

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$Rate        = 40      # requests per second, open loop
$Concurrency = 32
$LoadSeconds = 45
$Warmup      = 5
$TotalRun    = 100     # controller lifetime, covers ramp up, load and ramp down
$Size        = 384     # work size parameter
$Iter        = 500     # iteration ceiling

function Section($text) {
    Write-Host ""
    Write-Host ("=" * 78)
    Write-Host $text
    Write-Host ("=" * 78)
}

# Windows PowerShell turns a native program's stderr into error records, so redirecting it
# with *> makes a perfectly successful docker command look like a failure. Handing the
# whole command to cmd keeps the redirection on the process, where it belongs.
function Invoke-Quiet($command) {
    cmd /c "$command > NUL 2>&1"
    return $LASTEXITCODE
}

# --- 0. prerequisites -------------------------------------------------------
Section "0. Checking prerequisites"

if ((Invoke-Quiet "docker info") -ne 0) {
    throw "Docker is not running. Start it and run this script again."
}
Write-Host "docker            ok"

$bin = Join-Path $PSScriptRoot "build"
foreach ($exe in @("stratus-autoscaler.exe", "stratus-loadgen.exe", "stratus-cost.exe")) {
    if (-not (Test-Path (Join-Path $bin $exe))) {
        throw "$exe not found in build/. Run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build build"
    }
}
Write-Host "host binaries     ok"
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "results") | Out-Null

# --- 1. stack ---------------------------------------------------------------
Section "1. Starting the stack with one worker"

if ((Invoke-Quiet "docker compose up -d --build --scale worker=1") -ne 0) {
    throw "docker compose up failed"
}

$ready = $false
foreach ($attempt in 1..30) {
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:8080/healthz" -TimeoutSec 2 -UseBasicParsing
        if ($r.StatusCode -eq 200) { $ready = $true; break }
    } catch { Start-Sleep -Milliseconds 1000 }
}
if (-not $ready) { throw "the proxy did not become ready on port 8080" }

$backends = (Invoke-WebRequest -Uri "http://127.0.0.1:8080/backends" -UseBasicParsing).Content
Write-Host $backends

# --- 2. controller ----------------------------------------------------------
Section "2. Starting the autoscaling controller"

$log = Join-Path $PSScriptRoot "results\autoscaler.log"
$csv = Join-Path $PSScriptRoot "results\scaling-timeline.csv"
if (Test-Path $log) { Remove-Item $log }

$controller = Start-Process -FilePath (Join-Path $bin "stratus-autoscaler.exe") `
    -ArgumentList @(
        "--host", "127.0.0.1", "--port", "8080",
        "--min", "1", "--max", "6",
        "--target", "0.70",
        "--interval", "3",
        "--duration", "$TotalRun",
        "--cooldown-up", "6",
        "--cooldown-down", "15",
        "--csv", $csv
    ) `
    -WorkingDirectory $PSScriptRoot -NoNewWindow -PassThru -RedirectStandardOutput $log

Write-Host "controller pid    $($controller.Id)"
Write-Host "target            0.70 utilisation, 1 to 6 replicas, 3 s period"
Start-Sleep -Seconds 8

# --- 3. load ----------------------------------------------------------------
Section "3. Driving $Rate req/s at a fleet of one, which cannot absorb it"

& (Join-Path $bin "stratus-loadgen.exe") `
    --host 127.0.0.1 --port 8080 `
    --size $Size --iter $Iter `
    --concurrency $Concurrency --rate $Rate `
    --duration $LoadSeconds --warmup $Warmup `
    --csv (Join-Path $PSScriptRoot "results\latency.csv")

# --- 4. let it settle -------------------------------------------------------
Section "4. Load removed, waiting for the controller to release capacity"

while (-not $controller.HasExited) { Start-Sleep -Seconds 2 }

# --- 5. results -------------------------------------------------------------
Section "5. Scaling decisions, and the metric that triggered each one"
Get-Content $log

Section "6. Final fleet state"
(Invoke-WebRequest -Uri "http://127.0.0.1:8080/backends" -UseBasicParsing).Content

Section "7. Cost model"
Write-Host "No price is built into the calculator. The figures below are the placeholder"
Write-Host "inputs from this script, not a price list. Replace them with the prices you"
Write-Host "actually pay before quoting any result."
Write-Host ""
& (Join-Path $bin "stratus-cost.exe") `
    --price-instance-hour 0.04 `
    --instances 2 `
    --throughput 40 `
    --window 3600 `
    --currency "placeholder-unit"

Section "Done"
Write-Host "results\autoscaler.log        the timeline printed above"
Write-Host "results\scaling-timeline.csv  the same decisions, machine readable"
Write-Host "results\latency.csv           one row per request"
Write-Host "results\fleet-metrics.csv     the in-stack scraper's own record"
Write-Host ""
Write-Host "The stack is still running. Stop it with: docker compose down"
