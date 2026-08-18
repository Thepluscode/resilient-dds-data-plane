#!/usr/bin/env bash
# Runs each failure scenario as two real processes over RTPS and asserts the
# expected evidence appeared. A scenario that produces no evidence FAILS — a
# probe that can only pass is decoration.
set -uo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/evidence}
DOMAIN_BASE=${DOMAIN_BASE:-40}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
mkdir -p "$OUT"

pass=0; fail=0
domain=$DOMAIN_BASE

# run <name> <expected-grep> <sub-args...> -- <pub-args...>
run_scenario() {
    local name=$1 expect=$2; shift 2
    local sub_args=() pub_args=() cur=sub
    for a in "$@"; do
        if [[ $a == "--" ]]; then cur=pub; continue; fi
        if [[ $cur == sub ]]; then sub_args+=("$a"); else pub_args+=("$a"); fi
    done

    domain=$((domain + 1))   # isolate scenarios: a stale participant must not bleed across
    local log="$OUT/$name.log"

    "$SUB" --domain "$domain" --audit-out "$OUT/$name.jsonl" \
           --metrics-out "$OUT/$name.prom" "${sub_args[@]}" > "$log" 2>&1 &
    local sub_pid=$!
    sleep 1
    "$PUB" --domain "$domain" "${pub_args[@]}" >> "$log" 2>&1 &
    local pub_pid=$!
    wait $sub_pid; local sub_rc=$?
    kill $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null

    if [[ $sub_rc -ne 0 ]]; then
        echo "FAIL $name (subscriber exited $sub_rc)"; fail=$((fail + 1)); return
    fi
    if grep -qE "$expect" "$log"; then
        echo "PASS $name  <- $(grep -oE "$expect" "$log" | head -1)"
        pass=$((pass + 1))
    else
        echo "FAIL $name (no match for /$expect/ in $log)"
        tail -5 "$log" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
}

echo "== baseline: clean stream must produce NO anomalies =="
domain=$((domain + 1))
"$SUB" --domain $domain --duration-s 8 --metrics-out "$OUT/baseline.prom" \
       --audit-out "$OUT/baseline.jsonl" > "$OUT/baseline.log" 2>&1 &
sub_pid=$!
sleep 1
# The publisher must OUTLAST the subscriber's window. If it finishes first the
# trailing silence is a real deadline miss and the "healthy" baseline is not
# healthy; if it finishes at the same moment, the sample count races the clock.
"$PUB" --domain $domain --count 4000 --rate-hz 20 >> "$OUT/baseline.log" 2>&1 &
pub_pid=$!
wait $sub_pid
kill -9 $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null
received=$(grep -oE 'rdtf_samples_received_total [0-9]+' "$OUT/baseline.prom" | awk '{print $2}')
received=${received:-0}
anomalies=$(grep -c '^ANOMALY' "$OUT/baseline.log")
# Positive control: the clean run must actually MOVE data, or "0 anomalies" is vacuous.
if [[ $received -ge 100 && $anomalies -eq 0 ]]; then
    echo "PASS baseline  <- received=$received anomalies=0"; pass=$((pass + 1))
else
    echo "FAIL baseline (received=$received want>=100, anomalies=$anomalies want 0)"; fail=$((fail + 1))
fi

# Negative control for every status-callback scenario below. If these strings can
# appear in a healthy run, matching them later proves nothing.
nc_ok=1
for marker in DEADLINE_MISSED LIVELINESS_LOST INCOMPATIBLE_QOS; do
    if grep -q "$marker" "$OUT/baseline.log"; then
        echo "FAIL negative_control ($marker fired on a healthy stream)"
        fail=$((fail + 1)); nc_ok=0
    fi
done
if [[ $nc_ok -eq 1 ]]; then
    echo "PASS negative_control  <- no deadline/liveliness/QoS events on the healthy stream"
    pass=$((pass + 1))
fi

echo
echo "== failure injection =="
run_scenario packet_gap    'kind=sequence_gap missing=1' \
    --duration-s 8 -- --count 40 --rate-hz 20 --drop-at 20
run_scenario duplicate     'kind=duplicate' \
    --duration-s 8 -- --count 40 --rate-hz 20 --duplicate-at 20
run_scenario stale_sample  'kind=stale' \
    --duration-s 8 -- --count 40 --rate-hz 20 --stale-at 20
run_scenario schema_drift  'kind=schema_mismatch' \
    --duration-s 8 -- --count 40 --rate-hz 20 --schema-drift-at 20
run_scenario deadline_miss 'DEADLINE_MISSED' \
    --duration-s 10 -- --count 40 --rate-hz 20 --stall-at 2000

echo
echo "== late joiner (TRANSIENT_LOCAL replay) =="
# Two assertions, because either alone is vacuous: the reader must actually get
# the pre-join history (seq 1), AND the replayed history must be marked stale.
# Delivered is not the same as safe to use, and this is where that bites.
domain=$((domain + 1))
"$SUB" --domain $domain --duration-s 10 --start-delay-ms 3000 \
       --metrics-out "$OUT/late_joiner.prom" --audit-out "$OUT/late_joiner.jsonl" \
       > "$OUT/late_joiner.log" 2>&1 &
sub_pid=$!
sleep 0.2
"$PUB" --domain $domain --count 40 --rate-hz 10 --wait-match-ms 0 >> "$OUT/late_joiner.log" 2>&1
wait $sub_pid
first=$(grep -oE 'FIRST_SAMPLE seq=[0-9]+' "$OUT/late_joiner.log" | head -1 | cut -d= -f2)
stale=$(grep -oE 'rdtf_anomaly_stale_total [0-9]+' "$OUT/late_joiner.prom" | awk '{print $2}')
first=${first:-0}; stale=${stale:-0}
if [[ $first -eq 1 && $stale -ge 20 ]]; then
    echo "PASS late_joiner  <- first replayed seq=$first, $stale replayed samples flagged stale"
    pass=$((pass + 1))
else
    echo "FAIL late_joiner (first=$first want 1, stale=$stale want >=20)"; fail=$((fail + 1))
fi

echo
echo "== dead writer (SIGKILL, no goodbye message -> lease expiry) =="
# A clean exit unmatches the writer and the reader reports 0 matches, NOT a
# liveliness loss. Only an ungraceful death leaves the lease to expire, which is
# the case that matters: crashed process, severed link, wedged host.
domain=$((domain + 1))
"$SUB" --domain $domain --profile critical_control --duration-s 12 \
       --metrics-out "$OUT/dead_writer.prom" > "$OUT/dead_writer.log" 2>&1 &
sub_pid=$!
sleep 1
"$PUB" --domain $domain --profile critical_control --count 2000 --rate-hz 20 \
       >> "$OUT/dead_writer.log" 2>&1 &
pub_pid=$!
sleep 4
kill -9 $pub_pid 2>/dev/null; wait $pub_pid 2>/dev/null
wait $sub_pid
if grep -q 'LIVELINESS_LOST' "$OUT/dead_writer.log"; then
    echo "PASS dead_writer  <- $(grep -m1 LIVELINESS_LOST "$OUT/dead_writer.log")"
    pass=$((pass + 1))
else
    echo "FAIL dead_writer (lease never expired into a liveliness loss)"; fail=$((fail + 1))
fi

echo
echo "== QoS incompatibility (best-effort writer vs reliable reader) =="
domain=$((domain + 1))
"$SUB" --domain $domain --profile periodic_telemetry --duration-s 8 \
       --metrics-out "$OUT/qos_mismatch.prom" > "$OUT/qos_mismatch.log" 2>&1 &
sub_pid=$!
sleep 1
"$PUB" --domain $domain --profile high_rate_sensor --count 40 --rate-hz 20 \
       >> "$OUT/qos_mismatch.log" 2>&1
wait $sub_pid
if grep -q 'INCOMPATIBLE_QOS' "$OUT/qos_mismatch.log"; then
    echo "PASS qos_mismatch  <- $(grep -m1 INCOMPATIBLE_QOS "$OUT/qos_mismatch.log")"
    pass=$((pass + 1))
else
    echo "FAIL qos_mismatch (reader never reported incompatible QoS)"; fail=$((fail + 1))
fi

echo
echo "scenarios: $pass passed, $fail failed"
[[ $fail -eq 0 ]] || exit 1
