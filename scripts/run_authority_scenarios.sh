#!/usr/bin/env bash
# Milestone 4A: prove multi-writer visibility, exclusive ownership, hard-failure
# takeover, and freshness of the first standby sample.
set -euo pipefail

BUILD=${BUILD:-/tmp/b-authority}
OUT=${OUT:-/tmp/authority-evidence}
WRITER="$BUILD/resilientdds_authority_writer"
READER="$BUILD/resilientdds_authority_reader"
mkdir -p "$OUT"

[[ -x "$WRITER" && -x "$READER" ]] || { echo "authority binaries missing under $BUILD" >&2; exit 2; }

pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do kill -9 "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

wait_for() {
    local pattern=$1 file=$2 tries=${3:-100}
    for ((i=0; i<tries; ++i)); do
        grep -Fq "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "timeout waiting for '$pattern' in $file" >&2
    return 1
}

writer_handle() {
    awk '/WRITER_READY/ { for (i=1;i<=NF;i++) if ($i ~ /^handle=/) { sub(/^handle=/,"",$i); print $i; exit } }' "$1"
}

# ---------------------------------------------------------------------------
# Control: SHARED ownership must expose both writers to the reader. If this
# cannot see both handles, the EXCLUSIVE suppression test below is vacuous.
shared="$OUT/shared_control"
"$READER" --domain 181 --ownership shared --duration-s 5 >"$shared.reader.log" 2>&1 &
reader_pid=$!; pids+=("$reader_pid")
wait_for "READER_READY" "$shared.reader.log"

"$WRITER" --domain 181 --ownership shared --role shared-a --strength 0 \
    --count 1000 --rate-hz 50 >"$shared.a.log" 2>&1 &
a_pid=$!; pids+=("$a_pid")
"$WRITER" --domain 181 --ownership shared --role shared-b --strength 0 \
    --count 1000 --rate-hz 50 >"$shared.b.log" 2>&1 &
b_pid=$!; pids+=("$b_pid")
wait_for "WRITER_READY" "$shared.a.log"
wait_for "WRITER_READY" "$shared.b.log"
a_handle=$(writer_handle "$shared.a.log")
b_handle=$(writer_handle "$shared.b.log")
wait "$reader_pid"
kill -9 "$a_pid" "$b_pid" 2>/dev/null || true

if [[ -z "$a_handle" || -z "$b_handle" || "$a_handle" == "$b_handle" ]]; then
    echo "FAIL shared_control: writer handles missing or identical" >&2
    exit 1
fi
if ! grep -Fq "$a_handle" "$shared.reader.log" || ! grep -Fq "$b_handle" "$shared.reader.log"; then
    echo "FAIL shared_control: reader did not observe both writers" >&2
    exit 1
fi
shared_changes=$(grep -c '^OWNER_CHANGE ' "$shared.reader.log" || true)
if (( shared_changes < 2 )); then
    echo "FAIL shared_control: expected competing-writer visibility, changes=$shared_changes" >&2
    exit 1
fi
echo "PASS shared_control handles=2 owner_changes=$shared_changes"

# ---------------------------------------------------------------------------
# Exclusive: primary strength 100 must remain the only visible owner while a
# strength-10 standby is alive and matched. SIGKILL then forces a real failure;
# the first standby sample must arrive after the kill and still be fresh.
exclusive="$OUT/exclusive_failover"
"$READER" --domain 182 --ownership exclusive --duration-s 10 >"$exclusive.reader.log" 2>&1 &
reader_pid=$!; pids+=("$reader_pid")
wait_for "READER_READY" "$exclusive.reader.log"

"$WRITER" --domain 182 --ownership exclusive --role primary --strength 100 \
    --count 10000 --rate-hz 50 >"$exclusive.primary.log" 2>&1 &
primary_pid=$!; pids+=("$primary_pid")
wait_for "WRITER_READY" "$exclusive.primary.log"
primary_handle=$(writer_handle "$exclusive.primary.log")
wait_for "current=$primary_handle" "$exclusive.reader.log"

"$WRITER" --domain 182 --ownership exclusive --role standby --strength 10 \
    --count 10000 --rate-hz 50 >"$exclusive.standby.log" 2>&1 &
standby_pid=$!; pids+=("$standby_pid")
wait_for "WRITER_READY" "$exclusive.standby.log"
standby_handle=$(writer_handle "$exclusive.standby.log")
wait_for "MATCH writers=2" "$exclusive.reader.log"
sleep 1

if grep -Fq "current=$standby_handle" "$exclusive.reader.log"; then
    echo "FAIL exclusive_failover: lower-strength standby became visible before primary failure" >&2
    exit 1
fi

kill_ns=$(date +%s%N)
kill -9 "$primary_pid" 2>/dev/null || true
wait_for "current=$standby_handle" "$exclusive.reader.log" 80
change_line=$(grep -m1 -F "current=$standby_handle" "$exclusive.reader.log")
change_ns=$(sed -n 's/.*at_ns=\([0-9][0-9]*\).*/\1/p' <<<"$change_line")
age_ms=$(sed -n 's/.*age_ms=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' <<<"$change_line")

if [[ -z "$change_ns" || -z "$age_ms" ]]; then
    echo "FAIL exclusive_failover: could not parse takeover evidence" >&2
    exit 1
fi
if (( change_ns < kill_ns )); then
    echo "FAIL exclusive_failover: standby visible before hard failure" >&2
    exit 1
fi
failover_ms=$(( (change_ns - kill_ns) / 1000000 ))
if (( failover_ms > 1500 )); then
    echo "FAIL exclusive_failover: takeover too slow ${failover_ms}ms" >&2
    exit 1
fi
if (( age_ms < 0 || age_ms > 250 )); then
    echo "FAIL exclusive_failover: first standby sample unsafe age=${age_ms}ms" >&2
    exit 1
fi

wait "$reader_pid"
kill -9 "$standby_pid" 2>/dev/null || true

echo "PASS exclusive_primary_dominates primary=$primary_handle standby=$standby_handle"
echo "PASS exclusive_hard_failover failover_ms=$failover_ms first_standby_age_ms=$age_ms"

cat > "$OUT/matrix.md" <<MD
# Multi-writer authority evidence

| scenario | result | evidence |
|---|---|---|
| shared_control | PASS | both writer handles visible; owner_changes=$shared_changes |
| exclusive_primary_dominates | PASS | strength-10 standby stayed invisible while strength-100 primary was alive and both writers matched |
| exclusive_hard_failover | PASS | standby became visible after SIGKILL; failover_ms=$failover_ms; first_standby_age_ms=$age_ms |
MD

cat "$OUT/matrix.md"
