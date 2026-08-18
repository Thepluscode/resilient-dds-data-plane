#!/usr/bin/env bash
# Milestone 2: network degradation and recovery.
#
# Requires NET_ADMIN and the loopback qdisc, so it must run as:
#   docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-build ...
#
# Both endpoints run UDP-only (SHM disabled in the transport). If Fast DDS were
# left on its default transports it would take the shared-memory path between
# local processes and every impairment below would be silently bypassed --
# producing a table of clean results that proves nothing.
set -uo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/netem}
DEV=${DEV:-lo}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
RATE_HZ=${RATE_HZ:-50}
DURATION=${DURATION:-10}
mkdir -p "$OUT"

if ! tc qdisc show dev "$DEV" >/dev/null 2>&1; then
    echo "tc unavailable on $DEV - run with --cap-add=NET_ADMIN" >&2
    exit 2
fi

clear_netem() { tc qdisc del dev "$DEV" root 2>/dev/null; true; }
apply_netem() { clear_netem; [[ -n "$1" ]] && tc qdisc add dev "$DEV" root netem $1; }
trap clear_netem EXIT

domain=60
pass=0; fail=0

metric() { grep -oE "^$2 [0-9.]+" "$1" 2>/dev/null | awk '{print $2}' | head -1; }

# impairment_run <label> <profile> <netem-args>
impairment_run() {
    local label=$1 profile=$2 netem=$3
    domain=$((domain + 1))
    local tag="${label}_${profile}"
    local log="$OUT/$tag.log" prom="$OUT/$tag.prom"

    apply_netem "$netem"
    "$SUB" --domain $domain --profile "$profile" --duration-s "$DURATION" \
           --metrics-out "$prom" --audit-out "$OUT/$tag.jsonl" > "$log" 2>&1 &
    local sub_pid=$!
    sleep 1
    "$PUB" --domain $domain --profile "$profile" --count $((RATE_HZ * (DURATION + 20))) \
           --rate-hz "$RATE_HZ" >> "$log" 2>&1 &
    local pub_pid=$!
    wait $sub_pid
    kill -9 $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null
    clear_netem

    local recv gaps stale dl p50 p99 max
    recv=$(metric "$prom" rdtf_samples_received_total); recv=${recv:-0}
    gaps=$(metric "$prom" rdtf_anomaly_sequence_gap_total); gaps=${gaps:-0}
    stale=$(metric "$prom" rdtf_anomaly_stale_total); stale=${stale:-0}
    dl=$(metric "$prom" rdtf_deadline_missed_total); dl=${dl:-0}
    p50=$(metric "$prom" rdtf_e2e_latency_p50_us); p50=${p50:-0}
    p99=$(metric "$prom" rdtf_e2e_latency_p99_us); p99=${p99:-0}
    max=$(metric "$prom" rdtf_e2e_latency_max_us); max=${max:-0}

    printf '| %-12s | %-16s | %6s | %5s | %5s | %5s | %9s | %9s | %9s |\n' \
        "$label" "$profile" "$recv" "$gaps" "$stale" "$dl" "${p50%.*}" "${p99%.*}" "${max%.*}" \
        | tee -a "$OUT/matrix.md"

    # The only universal invariant: the run must have moved data. Anything
    # stronger differs per impairment and per QoS, which is the whole point.
    if [[ ${recv%.*} -gt 0 ]]; then pass=$((pass + 1)); else
        echo "FAIL $tag moved no data at all"; fail=$((fail + 1)); fi
}

echo "# Network degradation matrix (${RATE_HZ} Hz, ${DURATION}s, UDP-only on $DEV)" > "$OUT/matrix.md"
echo "" >> "$OUT/matrix.md"
echo '| impairment | profile | recv | gaps | stale | dl_miss | p50_us | p99_us | max_us |' | tee -a "$OUT/matrix.md"
echo '|---|---|---|---|---|---|---|---|---|' | tee -a "$OUT/matrix.md"

for profile in periodic_telemetry high_rate_sensor; do
    impairment_run baseline     "$profile" ""
    impairment_run loss_1pct    "$profile" "loss 1%"
    impairment_run loss_15pct   "$profile" "loss 15%"
    impairment_run delay_50ms   "$profile" "delay 50ms"
    impairment_run jitter_100ms "$profile" "delay 50ms 50ms distribution normal"
    impairment_run reorder_20pct "$profile" "delay 10ms reorder 20% 50%"
done

# ------------------------------------------------------------------ recovery
echo
echo "== partition and recovery =="
domain=$((domain + 1))
log="$OUT/partition.log"

"$SUB" --domain $domain --profile periodic_telemetry --duration-s 24 \
       --metrics-out "$OUT/partition.prom" > "$log" 2>&1 &
sub_pid=$!
sleep 1
"$PUB" --domain $domain --profile periodic_telemetry --count 100000 --rate-hz "$RATE_HZ" \
       >> "$log" 2>&1 &
pub_pid=$!
sleep 6
partition_abs=$(date +%s%3N)
apply_netem "loss 100%"
sleep 9
clear_netem
restore_abs=$(date +%s%3N)
wait $sub_pid
kill -9 $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null

# HEALTH_AT timestamps are relative to subscriber start, so convert the
# harness's own wall-clock fault marks into that frame rather than guessing.
python3 "$(dirname "$0")/analyze_recovery.py" "$log" "$partition_abs" "$restore_abs" \
    | tee -a "$OUT/matrix.md"

# --------------------------------------------------------- slow consumer
# The network is perfectly healthy here. The subscriber simply cannot keep up,
# which is a failure mode that looks nothing like loss: nothing is "offline",
# and the data still stops being usable.
echo
echo "== slow consumer (healthy network, overloaded reader) =="
for profile in periodic_telemetry high_rate_sensor; do
    domain=$((domain + 1))
    tag="slow_consumer_$profile"
    "$SUB" --domain $domain --profile "$profile" --duration-s 10 --process-delay-us 4000 \
           --metrics-out "$OUT/$tag.prom" > "$OUT/$tag.log" 2>&1 &
    sub_pid=$!
    sleep 1
    "$PUB" --domain $domain --profile "$profile" --count 100000 --rate-hz 500 \
           >> "$OUT/$tag.log" 2>&1 &
    pub_pid=$!
    wait $sub_pid
    kill -9 $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null

    recv=$(metric "$OUT/$tag.prom" rdtf_samples_received_total); recv=${recv:-0}
    lost=$(metric "$OUT/$tag.prom" rdtf_middleware_sample_lost_total); lost=${lost:-0}
    gaps=$(metric "$OUT/$tag.prom" rdtf_anomaly_sequence_gap_total); gaps=${gaps:-0}
    stale=$(metric "$OUT/$tag.prom" rdtf_anomaly_stale_total); stale=${stale:-0}
    p99=$(metric "$OUT/$tag.prom" rdtf_e2e_latency_p99_us); p99=${p99:-0}
    printf '| %-12s | %-16s | %6s | %5s | %5s | %5s | %9s | %9s | %9s |\n' \
        "slow_reader" "$profile" "$recv" "$gaps" "$stale" "-" "-" "${p99%.*}" "lost=${lost%.*}" \
        | tee -a "$OUT/matrix.md"
    if [[ ${recv%.*} -gt 0 ]]; then pass=$((pass + 1)); else
        echo "FAIL $tag moved no data"; fail=$((fail + 1)); fi
done

echo
echo "impairment runs: $pass ok, $fail failed"
echo "matrix written to $OUT/matrix.md"
[[ $fail -eq 0 ]] || exit 1
