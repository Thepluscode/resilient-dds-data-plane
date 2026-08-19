#!/usr/bin/env bash
# A ~3 minute live demo of the four results that are worth showing.
#
# Each act is a measurement, not a slide: it runs real DDS processes and prints
# what they actually produced. PAUSE=1 waits for a keypress between acts.
#
#   docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-fastdds:2.14.6 \
#     bash -c 'cmake -S . -B build-dds -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null \
#              && cmake --build build-dds -j >/dev/null && BUILD=build-dds ./scripts/demo.sh'
set -uo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/demo}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
PAUSE=${PAUSE:-0}
mkdir -p "$OUT"

[[ -x "$PUB" && -x "$SUB" ]] || { echo "binaries missing under $BUILD" >&2; exit 2; }
ldd "$PUB" 2>/dev/null | grep -q "not found" && { echo "ABORT: $PUB cannot load its libraries" >&2; exit 2; }

act()   { echo; echo "════════════════════════════════════════════════════════════"; echo " $1"; echo "════════════════════════════════════════════════════════════"; }
say()   { echo "  $*"; }
hold()  { [[ $PAUSE == 1 ]] && { echo; read -rp "  [enter] "; }; true; }
val()   { local v; v=$(awk -v m="$2" '$1 == m { print $2; exit }' "$1" 2>/dev/null); v=${v:-0}; echo "${v%.*}"; }

domain=150
netem() { tc qdisc del dev lo root 2>/dev/null; [[ -n "${1:-}" ]] && tc qdisc add dev lo root netem $1; true; }
trap 'netem; pkill -f resilientdds_dds_ 2>/dev/null' EXIT

# ─────────────────────────────────────────────────── ACT 1
act "1. A keyed topic silently loses two thirds of its state"
say "Publishing 32 distinct keys. Nothing is misconfigured, nothing errors."
domain=$((domain + 1))
"$SUB" --domain $domain --duration-s 6 --metrics-out "$OUT/a1.default.prom" >/dev/null 2>&1 &
s=$!; sleep 2
"$PUB" --domain $domain --count 900 --rate-hz 100 --keys 32 >/dev/null 2>&1 & p=$!
wait $s; kill -9 $p 2>/dev/null
d=$(grep -c by_key "$OUT/a1.default.prom" 2>/dev/null || echo 0)
say ""
say "  default max_instances   ->  $d / 32 keys delivered"
say ""
say "No error. No sample-lost callback. No anomaly. Every diagnostic green."
say "Fast DDS defaults ResourceLimitsQosPolicy.max_instances to 10."
hold
domain=$((domain + 1))
"$SUB" --domain $domain --duration-s 6 --max-instances 64 --metrics-out "$OUT/a1.raised.prom" >/dev/null 2>&1 &
s=$!; sleep 2
"$PUB" --domain $domain --count 900 --rate-hz 100 --keys 32 --max-instances 64 >/dev/null 2>&1 & p=$!
wait $s; kill -9 $p 2>/dev/null
r=$(grep -c by_key "$OUT/a1.raised.prom" 2>/dev/null || echo 0)
say "  max_instances = 64      ->  $r / 32 keys delivered"
say ""
say "The first run is the control. Without it the second proves nothing."
hold

# ─────────────────────────────────────────────────── ACT 2
act "2. Packet loss does not become gaps. It becomes latency."
say "15% loss on the wire. Same impairment, two QoS contracts."
for prof in periodic_telemetry high_rate_sensor; do
    domain=$((domain + 1))
    netem "loss 15%"
    "$SUB" --domain $domain --profile $prof --duration-s 6 --metrics-out "$OUT/a2.$prof.prom" >/dev/null 2>&1 &
    s=$!; sleep 1
    "$PUB" --domain $domain --profile $prof --count 900 --rate-hz 50 >/dev/null 2>&1 & p=$!
    wait $s; kill -9 $p 2>/dev/null; netem
    g=$(val "$OUT/a2.$prof.prom" rdtf_anomaly_sequence_gap_total)
    st=$(val "$OUT/a2.$prof.prom" rdtf_anomaly_stale_total)
    p50=$(val "$OUT/a2.$prof.prom" rdtf_e2e_latency_p50_us)
    label=$([[ $prof == periodic_telemetry ]] && echo "RELIABLE   " || echo "BEST_EFFORT")
    # Sub-millisecond latencies are the whole point of the best-effort row;
    # integer-dividing them to "0 ms" throws the comparison away.
    if [[ $p50 -ge 1000 ]]; then lat="$((p50 / 1000)) ms"; else lat="${p50} us"; fi
    say "  $label  gaps=$g  stale=$st  p50=$lat"
done
say ""
say "Reliable recovers every sample and spends the time budget doing it."
say "Best-effort drops samples and stays real-time. Neither is 'safer'."
hold

# ─────────────────────────────────────────────────── ACT 3
act "3. The consumer is unsafe long before DDS says anything"
say "Full partition mid-stream, then restore."
domain=$((domain + 1))
"$SUB" --domain $domain --duration-s 16 --metrics-out "$OUT/a3.prom" > "$OUT/a3.log" 2>&1 &
s=$!; sleep 1
"$PUB" --domain $domain --count 100000 --rate-hz 50 >/dev/null 2>&1 & p=$!
sleep 5; part=$(date +%s%3N); netem "loss 100%"
sleep 7; netem; rest=$(date +%s%3N)
wait $s; kill -9 $p 2>/dev/null
python3 "$(dirname "$0")/analyze_recovery.py" "$OUT/a3.log" "$part" "$rest" 2>/dev/null | sed -n '/time_to_unsafe/,$p' | sed 's/^/  /'
hold

# ─────────────────────────────────────────────────── ACT 4
act "4. write() returned true. The sample was still dropped."
say "Producer 400 Hz, consumer capped near 250/s. Same overload, two contracts."
for cfg in "keep_last 0 100 KEEP_LAST" "keep_all 64 100 KEEP_ALL_"; do
    set -- $cfg
    domain=$((domain + 1))
    "$SUB" --domain $domain --duration-s 6 --process-delay-us 4000 \
        --metrics-out "$OUT/a4.$1.prom" >/dev/null 2>&1 &
    s=$!; sleep 1
    "$PUB" --domain $domain --count 2400 --rate-hz 400 \
        --history "$1" --max-samples "$2" --max-blocking-ms "$3" > "$OUT/a4.$1.log" 2>&1
    wait $s
    to=$(grep -oE 'rdtf_publish_timeout_total [0-9]+' "$OUT/a4.$1.log" | awk '{print $2}' | head -1)
    gp=$(val "$OUT/a4.$1.prom" rdtf_anomaly_sequence_gap_total)
    say "  ${4//_/ }  write failures=${to:-0}   gaps seen by reader=$gp"
done
say ""
say "KEEP_LAST: the producer is told nothing while the consumer loses data."
say "KEEP_ALL:  the same loss arrives as backpressure it can act on."
echo
echo "  Full evidence: docs/ — every number above is reproduced by CI."
echo
