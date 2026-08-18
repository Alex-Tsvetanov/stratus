#!/usr/bin/env bash
# Stratus end to end demonstration. See demo.ps1 for the same script on Windows.
#
#   1. bring the stack up with a single worker
#   2. start the autoscaling controller on the host
#   3. drive open loop load at a rate one worker cannot absorb
#   4. stop the load and let the controller give the capacity back
#   5. print the decision timeline, the load report and the cost model
#
# Everything measured here is local. Nothing is deployed to any cloud account.
set -euo pipefail

cd "$(dirname "$0")"

RATE=40
CONCURRENCY=32
LOAD_SECONDS=45
WARMUP=5
TOTAL_RUN=100
SIZE=384
ITER=500

BIN=build
section() { printf '\n%s\n%s\n%s\n' "$(printf '=%.0s' {1..78})" "$1" "$(printf '=%.0s' {1..78})"; }

section "0. Checking prerequisites"
docker info >/dev/null 2>&1 || { echo "Docker is not running."; exit 1; }
echo "docker            ok"
for exe in stratus-autoscaler stratus-loadgen stratus-cost; do
    [ -x "$BIN/$exe" ] || { echo "$exe not found in build/. Run cmake first."; exit 1; }
done
echo "host binaries     ok"
mkdir -p results

section "1. Starting the stack with one worker"
docker compose up -d --build --scale worker=1 >/dev/null 2>&1
for _ in $(seq 1 30); do
    if curl -fsS http://127.0.0.1:8080/healthz >/dev/null 2>&1; then break; fi
    sleep 1
done
curl -fsS http://127.0.0.1:8080/backends

section "2. Starting the autoscaling controller"
rm -f results/autoscaler.log
"$BIN/stratus-autoscaler" \
    --host 127.0.0.1 --port 8080 \
    --min 1 --max 6 \
    --target 0.70 \
    --interval 3 \
    --duration "$TOTAL_RUN" \
    --cooldown-up 6 \
    --cooldown-down 15 \
    --csv results/scaling-timeline.csv > results/autoscaler.log 2>&1 &
CONTROLLER=$!
echo "controller pid    $CONTROLLER"
echo "target            0.70 utilisation, 1 to 6 replicas, 3 s period"
sleep 8

section "3. Driving $RATE req/s at a fleet of one, which cannot absorb it"
"$BIN/stratus-loadgen" \
    --host 127.0.0.1 --port 8080 \
    --size "$SIZE" --iter "$ITER" \
    --concurrency "$CONCURRENCY" --rate "$RATE" \
    --duration "$LOAD_SECONDS" --warmup "$WARMUP" \
    --csv results/latency.csv

section "4. Load removed, waiting for the controller to release capacity"
wait "$CONTROLLER" || true

section "5. Scaling decisions, and the metric that triggered each one"
cat results/autoscaler.log

section "6. Final fleet state"
curl -fsS http://127.0.0.1:8080/backends

section "7. Cost model"
cat <<'EOF'
No price is built into the calculator. The figures below are the placeholder inputs
from this script, not a price list. Replace them with the prices you actually pay
before quoting any result.
EOF
echo
"$BIN/stratus-cost" \
    --price-instance-hour 0.04 \
    --instances 2 \
    --throughput 40 \
    --window 3600 \
    --currency "placeholder-unit"

section "Done"
cat <<'EOF'
results/autoscaler.log        the timeline printed above
results/scaling-timeline.csv  the same decisions, machine readable
results/latency.csv           one row per request
results/fleet-metrics.csv     the in-stack scraper's own record

The stack is still running. Stop it with: docker compose down
EOF
