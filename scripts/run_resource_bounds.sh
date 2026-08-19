#!/usr/bin/env bash
# Milestone 4B: prove writer-side bounded-resource behavior under a matched
# RELIABLE reader that stops acknowledging data.
set -euo pipefail

BUILD=${BUILD:-/tmp/b}
OUT=${OUT:-/tmp/resource-bounds}
WRITER="$BUILD/resilientdds_resource_writer"
READER="$BUILD/resilientdds_resource_reader"
mkdir -p "$OUT"

reader_pid=""
writer_pid=""
cleanup() {
    if [[ -n "$reader_pid" ]]; then kill -CONT "$reader_pid" 2>/dev/null || true; kill "$reader_pid" 2>/dev/null || true; fi
    if [[ -n "$writer_pid" ]]; then kill "$writer_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT

wait_for() {
    local pattern=$1 file=$2 pid=$3 timeout_s=${4:-8}
    local deadline=$((SECONDS + timeout_s))
    while (( SECONDS < deadline )); do
        if grep -q "$pattern" "$file" 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "process $pid exited before '$pattern' appeared in $file" >&2
            cat "$file" >&2 || true
            return 1
        fi
        sleep 0.05
    done
    echo "timeout waiting for '$pattern' in $file" >&2
    cat "$file" >&2 || true
    return 1
}

summary_value() {
    local file=$1 key=$2
    grep 'RESOURCE_SUMMARY' "$file" | tail -1 | grep -oE "$key=[0-9]+" | cut -d= -f2
}

pass=0
fail=0
record_pass() { echo "PASS $1  <- $2"; pass=$((pass + 1)); }
record_fail() { echo "FAIL $1  <- $2"; fail=$((fail + 1)); }

# -------------------------------------------------------------- healthy control
# Tiny KEEP_ALL history by itself must not cause failure. With a healthy matched
# reader, ACKs should continuously release writer history and every write should
# complete without a resource timeout.
echo "== healthy reliable reader: bounded history must not timeout =="
domain=191
healthy_reader="$OUT/healthy.reader.log"
healthy_writer="$OUT/healthy.writer.log"
: > "$healthy_reader"; : > "$healthy_writer"
"$READER" --domain "$domain" --duration-s 20 > "$healthy_reader" 2>&1 & reader_pid=$!
wait_for RESOURCE_READER_READY "$healthy_reader" "$reader_pid"
"$WRITER" --domain "$domain" --count 200 --rate-hz 200 --history-limit 8 --max-blocking-ms 50 \
    > "$healthy_writer" 2>&1 & writer_pid=$!
wait "$writer_pid"; writer_pid=""
kill "$reader_pid" 2>/dev/null || true; wait "$reader_pid" 2>/dev/null || true; reader_pid=""

healthy_timeouts=$(summary_value "$healthy_writer" timeouts); healthy_timeouts=${healthy_timeouts:-999}
healthy_errors=$(summary_value "$healthy_writer" errors); healthy_errors=${healthy_errors:-999}
healthy_success=$(summary_value "$healthy_writer" success); healthy_success=${healthy_success:-0}
if [[ "$healthy_timeouts" -eq 0 && "$healthy_errors" -eq 0 && "$healthy_success" -eq 200 ]]; then
    record_pass healthy_control "success=$healthy_success timeouts=0 errors=0"
else
    record_fail healthy_control "success=$healthy_success timeouts=$healthy_timeouts errors=$healthy_errors"
fi

# ---------------------------------------------------------- frozen matched reader
# SIGSTOP is intentional: unlike SIGKILL it leaves the match/reader obligation in
# place while the remote process stops running protocol threads and therefore
# stops acknowledging reliable samples.
echo
echo "== frozen matched reader: writer history must exhaust and recover =="
domain=192
stall_reader="$OUT/stall.reader.log"
stall_writer="$OUT/stall.writer.log"
: > "$stall_reader"; : > "$stall_writer"
"$READER" --domain "$domain" --duration-s 30 > "$stall_reader" 2>&1 & reader_pid=$!
wait_for RESOURCE_READER_READY "$stall_reader" "$reader_pid"
"$WRITER" --domain "$domain" --count 600 --rate-hz 200 --history-limit 8 --max-blocking-ms 50 \
    > "$stall_writer" 2>&1 & writer_pid=$!
wait_for RESOURCE_WRITER_MATCHED "$stall_writer" "$writer_pid"
sleep 0.30
stall_start_ms=$(date +%s%3N)
kill -STOP "$reader_pid"
sleep 1.20
resume_ms=$(date +%s%3N)
kill -CONT "$reader_pid"
wait "$writer_pid"; writer_pid=""
sleep 0.20
kill "$reader_pid" 2>/dev/null || true; wait "$reader_pid" 2>/dev/null || true; reader_pid=""

stall_timeouts=$(summary_value "$stall_writer" timeouts); stall_timeouts=${stall_timeouts:-0}
stall_errors=$(summary_value "$stall_writer" errors); stall_errors=${stall_errors:-999}
stall_max_us=$(summary_value "$stall_writer" max_write_us); stall_max_us=${stall_max_us:-999999999}
stall_recovered=$(summary_value "$stall_writer" recovered); stall_recovered=${stall_recovered:-0}
first_timeout_ms=$(grep 'WRITE_TIMEOUT' "$stall_writer" | head -1 | grep -oE 'wall_ms=[0-9]+' | cut -d= -f2 || true)
recovered_ms=$(grep 'WRITE_RECOVERED' "$stall_writer" | head -1 | grep -oE 'wall_ms=[0-9]+' | cut -d= -f2 || true)

if [[ "$stall_timeouts" -gt 0 && "$stall_errors" -eq 0 ]]; then
    record_pass writer_history_exhaustion "timeouts=$stall_timeouts history_limit=8"
else
    record_fail writer_history_exhaustion "timeouts=$stall_timeouts errors=$stall_errors"
fi

# 50 ms is the configured max_blocking_time. Allow CI scheduling noise, but fail
# if writes block indefinitely or return immediately without reaching the budget.
if [[ "$stall_max_us" -ge 30000 && "$stall_max_us" -le 250000 ]]; then
    record_pass bounded_write_block "max_write_us=$stall_max_us configured_ms=50"
else
    record_fail bounded_write_block "max_write_us=$stall_max_us expected=30000..250000"
fi

if [[ -n "$first_timeout_ms" && "$first_timeout_ms" -ge "$stall_start_ms" && "$first_timeout_ms" -le $((resume_ms + 250)) ]]; then
    record_pass timeout_causality "first_timeout_wall_ms=$first_timeout_ms stall=$stall_start_ms..$resume_ms"
else
    record_fail timeout_causality "first_timeout_wall_ms=${first_timeout_ms:-none} stall=$stall_start_ms..$resume_ms"
fi

if [[ "$stall_recovered" -eq 1 && -n "$recovered_ms" && "$recovered_ms" -ge "$resume_ms" ]]; then
    recovery_ms=$((recovered_ms - resume_ms))
    if [[ "$recovery_ms" -le 1500 ]]; then
        record_pass writer_recovery "resume_to_write_recovery_ms=$recovery_ms"
    else
        record_fail writer_recovery "resume_to_write_recovery_ms=$recovery_ms > 1500"
    fi
else
    record_fail writer_recovery "recovered=$stall_recovered recovered_wall_ms=${recovered_ms:-none} resume_ms=$resume_ms"
fi

cat > "$OUT/matrix.md" <<EOF
# Writer resource-bounds matrix

| scenario | history | max_blocking | result |
|---|---:|---:|---|
| healthy matched RELIABLE reader | 8 KEEP_ALL | 50 ms | success=$healthy_success, timeouts=$healthy_timeouts |
| SIGSTOP matched RELIABLE reader | 8 KEEP_ALL | 50 ms | timeouts=$stall_timeouts, max_write_us=$stall_max_us |
| SIGCONT recovery | 8 KEEP_ALL | 50 ms | recovered=$stall_recovered, resume_to_recovery_ms=${recovery_ms:-n/a} |

The writer-side timeout is only claimed when Fast DDS returns RETCODE_TIMEOUT from
DataWriter::write. A slow application callback is a different overload layer and
remains covered by the existing reader-side slow-consumer scenario.
EOF

echo
echo "resource-bound scenarios: $pass passed, $fail failed"
echo "matrix written to $OUT/matrix.md"
[[ "$fail" -eq 0 ]]
