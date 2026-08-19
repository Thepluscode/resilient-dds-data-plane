#!/usr/bin/env bash
# Milestone 4C: fan-out and resource isolation.
#
# Every experiment so far used one publisher, one subscriber, one keyed
# instance. This asks the question that shape cannot answer:
#
#   can ONE unhealthy consumer, or ONE hot keyed instance, damage unrelated
#   healthy consumers and unrelated state?
#
# Isolation is the property under test, not throughput.
set -uo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/fanout}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
READERS=${READERS:-8}
RATE_HZ=${RATE_HZ:-100}
DURATION=${DURATION:-10}
mkdir -p "$OUT"

[[ -x "$PUB" && -x "$SUB" ]] || { echo "binaries missing under $BUILD" >&2; exit 2; }
for bin in "$PUB" "$SUB"; do
    if ldd "$bin" 2>/dev/null | grep -q "not found"; then
        echo "ABORT: $bin cannot load its shared libraries:" >&2
        ldd "$bin" 2>/dev/null | grep "not found" | sed 's/^/    /' >&2
        exit 2
    fi
done

pass=0; fail=0
# Fast DDS rejects domainId > 232, so the whole suite must stay well under it.
domain=100
pids=()
cleanup() { for p in "${pids[@]:-}"; do kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT

num() { local v; v=$(awk -v m="$2" '$1 == m { print $2; exit }' "$1" 2>/dev/null); echo "${v%.*}"; }
num0() { local v; v=$(num "$1" "$2"); echo "${v:-0}"; }
rss_kb() { awk '/VmRSS/ {print $2; exit}' "/proc/$1/status" 2>/dev/null || echo 0; }

ok()   { echo "PASS $1  <- $2"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1 ($2)"; fail=$((fail + 1)); }

# ---------------------------------------------------------------- S1 fan-out
echo "== S1: $READERS healthy readers =="
domain=$((domain + 1)); d1=$domain
for i in $(seq 1 "$READERS"); do
    "$SUB" --domain $d1 --duration-s "$DURATION" --metrics-out "$OUT/s1.r$i.prom" \
        > "$OUT/s1.r$i.log" 2>&1 &
    pids+=($!)
done
sleep 2
"$PUB" --domain $d1 --count $((RATE_HZ * (DURATION + 10))) --rate-hz "$RATE_HZ" \
    > "$OUT/s1.pub.log" 2>&1 &
s1_pub=$!; pids+=($s1_pub)
wait $(jobs -p 2>/dev/null | head -"$READERS") 2>/dev/null
sleep "$DURATION"
kill -9 $s1_pub 2>/dev/null

s1_min=999999; s1_max=0; s1_gaps=0; s1_p99max=0
for i in $(seq 1 "$READERS"); do
    r=$(num0 "$OUT/s1.r$i.prom" rdtf_samples_received_total)
    g=$(num0 "$OUT/s1.r$i.prom" rdtf_anomaly_sequence_gap_total)
    p=$(num0 "$OUT/s1.r$i.prom" rdtf_e2e_latency_p99_us)
    [[ $r -lt $s1_min ]] && s1_min=$r
    [[ $r -gt $s1_max ]] && s1_max=$r
    s1_gaps=$((s1_gaps + g))
    [[ $p -gt $s1_p99max ]] && s1_p99max=$p
done
echo "  per-reader received: min=$s1_min max=$s1_max  gaps=$s1_gaps  worst p99=${s1_p99max}us"
echo "s1 min=$s1_min max=$s1_max gaps=$s1_gaps p99=$s1_p99max" > "$OUT/raw.txt"

# Positive control first: the run must have moved real data to every reader.
if [[ $s1_min -lt 100 ]]; then
    bad healthy_fanout "a reader got only $s1_min samples; fan-out did not happen"
elif [[ $s1_gaps -ne 0 ]]; then
    bad healthy_fanout "$s1_gaps sequence gaps across $READERS healthy readers"
else
    ok healthy_fanout "$READERS readers, min=$s1_min max=$s1_max, 0 gaps, worst p99=${s1_p99max}us"
fi

# --------------------------------------------------------- S2 frozen reader
echo
echo "== S2: $((READERS - 1)) healthy + 1 SIGSTOP frozen reader =="
domain=$((domain + 1)); d2=$domain
s2_pids=()
for i in $(seq 1 "$READERS"); do
    "$SUB" --domain $d2 --duration-s "$DURATION" --metrics-out "$OUT/s2.r$i.prom" \
        > "$OUT/s2.r$i.log" 2>&1 &
    s2_pids+=($!); pids+=($!)
done
sleep 2
"$PUB" --domain $d2 --count $((RATE_HZ * (DURATION + 10))) --rate-hz "$RATE_HZ" \
    > "$OUT/s2.pub.log" 2>&1 &
s2_pub=$!; pids+=($s2_pub)
sleep 3
# SIGSTOP, not a slow loop: the reader stops draining entirely while staying
# matched and alive to DDS. That is the case a RELIABLE writer must absorb.
frozen_pid=${s2_pids[$((READERS - 1))]}
kill -STOP "$frozen_pid" 2>/dev/null && echo "  froze reader $READERS (pid $frozen_pid)"
sleep $((DURATION - 3))
kill -CONT "$frozen_pid" 2>/dev/null
sleep 2
kill -9 $s2_pub 2>/dev/null
for p in "${s2_pids[@]}"; do wait "$p" 2>/dev/null; done

s2_healthy_min=999999; s2_frozen=0
for i in $(seq 1 $((READERS - 1))); do
    r=$(num0 "$OUT/s2.r$i.prom" rdtf_samples_received_total)
    [[ $r -lt $s2_healthy_min ]] && s2_healthy_min=$r
done
s2_frozen=$(num0 "$OUT/s2.r$READERS.prom" rdtf_samples_received_total)
degradation=$(( s1_min > 0 ? (100 - (s2_healthy_min * 100 / s1_min)) : 100 ))
echo "  healthy min=$s2_healthy_min (baseline $s1_min, degradation ${degradation}%)  frozen=$s2_frozen"
echo "s2 healthy_min=$s2_healthy_min frozen=$s2_frozen degradation=$degradation" >> "$OUT/raw.txt"

# Control: the freeze must actually have starved the frozen reader, or "healthy
# readers were fine" is a statement about a fault that never happened.
if [[ $s2_frozen -ge $s2_healthy_min ]]; then
    bad fault_isolation "frozen reader got $s2_frozen vs healthy $s2_healthy_min - the freeze did nothing"
elif [[ $degradation -gt 25 ]]; then
    bad fault_isolation "healthy readers lost ${degradation}% vs baseline - one frozen reader coupled to the rest"
else
    ok fault_isolation "frozen=$s2_frozen starved, healthy min=$s2_healthy_min, degradation ${degradation}%"
fi

# ------------------------------------------------------------ S3 many keys
# Asks whether resource limits are global or per-instance, and answers it by
# running the SAME 32-key workload twice. The default run is the control: if it
# also delivered 32, the raised run would prove nothing.
echo
echo "== S3: 32 keyed instances, default vs raised max_instances =="
run_keys() {
    local extra=$1 tag=$2
    domain=$((domain + 1))
    "$SUB" --domain $domain --duration-s "$DURATION" $extra --metrics-out "$OUT/s3.$tag.prom" \
        > "$OUT/s3.$tag.log" 2>&1 &
    local sp=$!; pids+=($sp)
    sleep 2
    "$PUB" --domain $domain --count $((RATE_HZ * (DURATION + 10))) --rate-hz "$RATE_HZ" \
        --keys 32 $extra > "$OUT/s3.$tag.pub.log" 2>&1 &
    local pp=$!; pids+=($pp)
    wait $sp
    kill -9 $pp 2>/dev/null
    grep -c 'rdtf_samples_received_by_key' "$OUT/s3.$tag.prom" 2>/dev/null || echo 0
}
keys_default=$(run_keys "" default)
keys_raised=$(run_keys "--max-instances 64" raised)
echo "  default max_instances: $keys_default / 32 keys"
echo "  max_instances=64:      $keys_raised / 32 keys"
echo "s3 default=$keys_default raised=$keys_raised" >> "$OUT/raw.txt"

if [[ $keys_default -ne 10 ]]; then
    bad instance_limit_is_silent "expected Fast DDS default of 10 instances, saw $keys_default"
elif [[ $keys_raised -ne 32 ]]; then
    bad many_keys "raising max_instances to 64 still delivered only $keys_raised of 32"
else
    ok instance_limit_is_silent "default silently caps at $keys_default of 32 keys, no error raised"
    ok many_keys "max_instances=64 delivers all $keys_raised keys"
fi
keys_seen=$keys_raised

# --------------------------------------------------------- S4 key isolation
echo
echo "== S4: 1 hot key + 7 normal keys =="
domain=$((domain + 1)); d4=$domain
"$SUB" --domain $d4 --duration-s "$DURATION" --metrics-out "$OUT/s4.prom" > "$OUT/s4.log" 2>&1 &
s4_sub=$!; pids+=($s4_sub)
sleep 2
"$PUB" --domain $d4 --count $((RATE_HZ * (DURATION + 10))) --rate-hz "$RATE_HZ" \
    --keys 8 --hot-key 3 --hot-multiplier 20 > "$OUT/s4.pub.log" 2>&1 &   # 8 keys stays under the default 10
s4_pub=$!; pids+=($s4_pub)
wait $s4_sub
kill -9 $s4_pub 2>/dev/null

hot=$(num0 "$OUT/s4.prom" 'rdtf_samples_received_by_key{key="key-3"}')
cold_min=999999
for k in 0 1 2 4 5 6 7; do
    v=$(num0 "$OUT/s4.prom" "rdtf_samples_received_by_key{key=\"key-$k\"}")
    [[ $v -lt $cold_min ]] && cold_min=$v
done
cold_stale=$(grep -c 'rdtf_anomaly_by_key{kind="stale",key="key-[0124567]"}' "$OUT/s4.prom" 2>/dev/null)
cold_stale=${cold_stale:-0}
echo "  hot key-3=$hot  coldest normal key=$cold_min  normal keys with staleness=$cold_stale"
echo "s4 hot=$hot cold_min=$cold_min cold_stale=$cold_stale" >> "$OUT/raw.txt"

# Control: the hot key must be MEASURABLY hot, not merely one sample ahead.
# Round-robin ordering alone puts key-3 a sample or two above its neighbours, so
# a bare `hot > cold_min` test passes with the overload switched off entirely --
# verified by mutation. The multiplier is 20, so demand at least 5x.
HOT_RATIO_MIN=5
if [[ $hot -lt $((cold_min * HOT_RATIO_MIN)) ]]; then
    bad key_isolation "hot key got $hot vs normal $cold_min, under ${HOT_RATIO_MIN}x - the overload never happened"
elif [[ $cold_min -lt 50 ]]; then
    bad key_isolation "normal keys starved to $cold_min while key-3 got $hot"
elif [[ $cold_stale -ne 0 ]]; then
    bad key_isolation "$cold_stale normal keys went stale beside the hot key"
else
    ok key_isolation "hot=$hot vs normal>=$cold_min, no staleness on unrelated keys"
fi

# ------------------------------------------------------ S5 reader churn/RSS
echo
echo "== S5: reader churn, writer resource reclamation =="
domain=$((domain + 1)); d5=$domain
"$PUB" --domain $d5 --count 1000000 --rate-hz "$RATE_HZ" --wait-match-ms 0 \
    > "$OUT/s5.pub.log" 2>&1 &
s5_pub=$!; pids+=($s5_pub)
sleep 2
rss_base=$(rss_kb $s5_pub)
for cycle in 1 2 3 4 5; do
    "$SUB" --domain $d5 --duration-s 2 --metrics-out "$OUT/s5.c$cycle.prom" \
        > "$OUT/s5.c$cycle.log" 2>&1 &
    c=$!
    sleep 1
    kill -9 $c 2>/dev/null; wait $c 2>/dev/null
    sleep 1
done
sleep 3
rss_after=$(rss_kb $s5_pub)
still_writing=$(grep -c 'rdtf_samples_published_total' "$OUT/s5.pub.log" 2>/dev/null)
alive=$(kill -0 $s5_pub 2>/dev/null && echo yes || echo no)
growth=$(( rss_base > 0 ? (rss_after - rss_base) * 100 / rss_base : 0 ))
echo "  writer RSS ${rss_base}kB -> ${rss_after}kB (${growth}%)  alive=$alive"
echo "s5 rss_base=$rss_base rss_after=$rss_after growth=$growth alive=$alive" >> "$OUT/raw.txt"
kill -9 $s5_pub 2>/dev/null

if [[ "$alive" != "yes" ]]; then
    bad resource_reclamation "writer died during reader churn"
elif [[ $rss_base -eq 0 ]]; then
    bad resource_reclamation "could not read writer RSS - measurement absent, not a pass"
elif [[ $growth -gt 50 ]]; then
    bad resource_reclamation "writer RSS grew ${growth}% across 5 reader churn cycles"
else
    ok resource_reclamation "writer survived 5 churn cycles, RSS ${rss_base}->${rss_after}kB (${growth}%)"
fi

echo
echo "fan-out: $pass passed, $fail failed"
{
  echo "# Fan-out and resource isolation"
  echo ""
  echo "| scenario | result |"
  echo "|---|---|"
  echo "| $READERS healthy readers | min=$s1_min max=$s1_max gaps=$s1_gaps worst p99=${s1_p99max}us |"
  echo "| 1 of $READERS frozen | healthy min=$s2_healthy_min, frozen=$s2_frozen, degradation ${degradation}% |"
  echo "| 32 keyed instances | default $keys_default/32, max_instances=64 $keys_raised/32 |"
  echo "| hot key + 7 normal | hot=$hot, coldest normal=$cold_min, stale normal keys=$cold_stale |"
  echo "| reader churn x5 | writer RSS ${rss_base}->${rss_after}kB (${growth}%), alive=$alive |"
} > "$OUT/matrix.md"
cat "$OUT/matrix.md"
[[ $fail -eq 0 ]] || exit 1
