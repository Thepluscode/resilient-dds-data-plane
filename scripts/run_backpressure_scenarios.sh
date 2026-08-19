#!/usr/bin/env bash
# Milestone 4B: writer-side bounded resources.
#
# Milestone 2's slow-consumer scenario watched the READER and could only report
# that data arrived late and stale. It could not answer the producer's question:
# when the reader stops draining and the writer's history fills, what does
# write() do? That answer is a QoS choice, and it decides whether the producer
# ever learns it is losing data.
set -uo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/backpressure}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
RATE_HZ=${RATE_HZ:-400}
DURATION=${DURATION:-10}
# Reader deliberately slower than the writer: 4 ms per sample caps it near
# 250/s against a 400 Hz producer.
READER_DELAY_US=${READER_DELAY_US:-4000}
mkdir -p "$OUT"

[[ -x "$PUB" && -x "$SUB" ]] || { echo "binaries missing under $BUILD" >&2; exit 2; }

pass=0; fail=0
domain=200
metric() { awk -v m="$2" '$1 == m { print $2; exit }' "$1" 2>/dev/null; }
num() { local v; v=$(metric "$1" "$2"); v=${v:-0}; echo "${v%.*}"; }

# run <label> <history> <max_samples> <max_blocking_ms>
run_case() {
    local label=$1 hist=$2 maxs=$3 blockms=$4
    domain=$((domain + 1))
    local log="$OUT/$label.log" sprom="$OUT/$label.sub.prom" pprom="$OUT/$label.pub.prom"

    "$SUB" --domain $domain --profile periodic_telemetry --duration-s "$DURATION" \
           --process-delay-us "$READER_DELAY_US" --metrics-out "$sprom" \
           > "$OUT/$label.sub.log" 2>&1 &
    local sub_pid=$!
    sleep 1
    "$PUB" --domain $domain --profile periodic_telemetry \
           --count $((RATE_HZ * DURATION)) --rate-hz "$RATE_HZ" \
           --history "$hist" --max-samples "$maxs" --max-blocking-ms "$blockms" \
           > "$log" 2>&1
    wait $sub_pid

    local sent failed blocked_max recv gaps
    sent=$(grep -oE 'rdtf_samples_published_total [0-9]+' "$log" | awk '{print $2}' | head -1); sent=${sent:-0}
    failed=$(grep -oE 'rdtf_publish_failures_total [0-9]+' "$log" | awk '{print $2}' | head -1); failed=${failed:-0}
    blocked_max=$(grep -oE 'blocked_max_us=[0-9]+' "$log" | cut -d= -f2 | head -1); blocked_max=${blocked_max:-0}
    recv=$(num "$sprom" rdtf_samples_received_total)
    gaps=$(num "$sprom" rdtf_anomaly_sequence_gap_total)

    printf '| %-22s | %8s | %8s | %8s | %11s | %6s | %6s |\n' \
        "$label" "$hist" "$sent" "$failed" "$blocked_max" "$recv" "$gaps" | tee -a "$OUT/matrix.md"

    echo "$label sent=$sent failed=$failed blocked_max_us=$blocked_max recv=$recv gaps=$gaps" >> "$OUT/raw.txt"
    if [[ $((sent + failed)) -gt 0 ]]; then pass=$((pass + 1)); else
        echo "FAIL $label: writer never attempted a write"; fail=$((fail + 1)); fi
}

echo "# Writer-side bounded resources (${RATE_HZ} Hz producer, reader capped ~$((1000000 / READER_DELAY_US))/s)" > "$OUT/matrix.md"
echo "" >> "$OUT/matrix.md"
echo '| case | history | written | write_fail | blocked_max_us | recv | gaps |' | tee -a "$OUT/matrix.md"
echo '|---|---|---|---|---|---|---|' | tee -a "$OUT/matrix.md"
: > "$OUT/raw.txt"

run_case keep_last_depth32   keep_last 0   100
run_case keep_all_64         keep_all  64  100
run_case keep_all_64_noblock keep_all  64  0
run_case keep_all_512        keep_all  512 100

# ---------------------------------------------------------------- assertions
kl_fail=$(grep '^keep_last_depth32 ' "$OUT/raw.txt" | grep -oE 'failed=[0-9]+' | cut -d= -f2)
kl_gaps=$(grep '^keep_last_depth32 ' "$OUT/raw.txt" | grep -oE 'gaps=[0-9]+' | cut -d= -f2)
ka_fail=$(grep '^keep_all_64 ' "$OUT/raw.txt" | grep -oE 'failed=[0-9]+' | cut -d= -f2)
ka_block=$(grep '^keep_all_64 ' "$OUT/raw.txt" | grep -oE 'blocked_max_us=[0-9]+' | cut -d= -f2)

echo
echo "== assertions =="

# KEEP_LAST must lose data WITHOUT telling the producer. Both halves matter: a
# run with no gaps proves nothing (the reader kept up), and a run with write
# failures would mean the producer was in fact informed.
if [[ ${kl_gaps:-0} -gt 0 && ${kl_fail:-0} -eq 0 ]]; then
    echo "PASS keep_last_silent_loss  <- gaps=$kl_gaps reached the reader, write failures=$kl_fail"
    pass=$((pass + 1))
else
    echo "FAIL keep_last_silent_loss (gaps=$kl_gaps want >0, write_fail=$kl_fail want 0)"
    fail=$((fail + 1))
fi

# KEEP_ALL must push back on the producer, as blocking or as failed writes.
# The blocking floor is deliberately far above ordinary write latency: an
# unloaded run blocks ~1.2 ms, which a 1 ms threshold would happily accept as
# "backpressure" on a system under no pressure at all. Real history-full
# blocking runs to max_blocking_ms (100 ms here), so 50 ms separates them.
BLOCK_FLOOR_US=50000
if [[ ${ka_fail:-0} -gt 0 || ${ka_block:-0} -gt $BLOCK_FLOOR_US ]]; then
    echo "PASS keep_all_backpressure  <- write failures=$ka_fail, max blocked=${ka_block}us"
    pass=$((pass + 1))
else
    echo "FAIL keep_all_backpressure (failures=$ka_fail, blocked_max=${ka_block}us < ${BLOCK_FLOOR_US}us - producer never noticed)"
    fail=$((fail + 1))
fi

# Negative control: the QoS guard must refuse an unbounded keep-all writer
# BEFORE it can run. Without this the validate() rule could silently not fire
# and the matrix above would still look correct.
domain=$((domain + 1))
if "$PUB" --domain $domain --profile periodic_telemetry --count 10 --rate-hz 50 \
        --history keep_all --max-samples 0 --max-blocking-ms 100 \
        > "$OUT/unbounded_refused.log" 2>&1; then
    echo "FAIL unbounded_keep_all_refused: writer started with unlimited keep-all history"
    fail=$((fail + 1))
elif grep -q "keep-all reliable history needs max_samples" "$OUT/unbounded_refused.log"; then
    echo "PASS unbounded_keep_all_refused  <- $(grep -m1 'keep-all reliable' "$OUT/unbounded_refused.log")"
    pass=$((pass + 1))
else
    echo "FAIL unbounded_keep_all_refused: writer failed, but not for the expected reason"
    tail -3 "$OUT/unbounded_refused.log" | sed 's/^/      /'
    fail=$((fail + 1))
fi

echo
echo "backpressure: $pass passed, $fail failed"
cat "$OUT/matrix.md"
[[ $fail -eq 0 ]] || exit 1
