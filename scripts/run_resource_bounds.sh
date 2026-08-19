#!/usr/bin/env bash
# Milestone 4B: distinguish writer-pool exhaustion from bounded reliable-history
# backpressure when a matched reader stops acknowledging data.
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
# One extra sample is a deliberate reservoir outside max_samples. It allows a
# ninth change to be allocated while history is full, so a healthy reader has a
# chance to ACK and release history before max_blocking_time expires.
echo "== healthy reliable reader: bounded history plus reservoir =="
domain=191
healthy_reader="$OUT/healthy.reader.log"
healthy_writer="$OUT/healthy.writer.log"
: > "$healthy_reader"; : > "$healthy_writer"
"$READER" --domain "$domain" --duration-s 20 > "$healthy_reader" 2>&1 & reader_pid=$!
wait_for RESOURCE_READER_READY "$healthy_reader" "$reader_pid"
"$WRITER" --domain "$domain" --count 200 --rate-hz 200 --history-limit 8 --extra-samples 1 --max-blocking-ms 50 \
    > "$healthy_writer" 2>&1 & writer_pid=$!
wait "$writer_pid"; writer_pid=""
kill "$reader_pid" 2>/dev/null || true; wait "$reader_pid" 2>/dev/null || true; reader_pid=""

healthy_timeouts=$(summary_value "$healthy_writer" timeouts); healthy_timeouts=${healthy_timeouts:-999}
healthy_oor=$(summary_value "$healthy_writer" out_of_resources); healthy_oor=${healthy_oor:-999}
healthy_errors=$(summary_value "$healthy_writer" errors); healthy_errors=${healthy_errors:-999}
healthy_success=$(summary_value "$healthy_writer" success); healthy_success=${healthy_success:-0}
if [[ "$healthy_timeouts" -eq 0 && "$healthy_oor" -eq 0 && "$healthy_errors" -eq 0 && "$healthy_success" -eq 200 ]]; then
    record_pass healthy_control "success=$healthy_success timeouts=0 out_of_resources=0 errors=0"
else
    record_fail healthy_control "success=$healthy_success timeouts=$healthy_timeouts out_of_resources=$healthy_oor errors=$healthy_errors"
fi

# ---------------------------------------------------------- hard pool exhaustion
# extra_samples=0 removes the reservoir. Freeze the matched reader before writes
# begin. Once eight unacknowledged changes occupy the pool, the next allocation
# should fail immediately with RETCODE_OUT_OF_RESOURCES rather than waiting on
# max_blocking_time.
echo
echo "== frozen matched reader: zero reservoir -> hard pool exhaustion =="
domain=192
pool_reader="$OUT/pool.reader.log"
pool_writer="$OUT/pool.writer.log"
: > "$pool_reader"; : > "$pool_writer"
"$READER" --domain "$domain" --duration-s 20 > "$pool_reader" 2>&1 & reader_pid=$!
wait_for RESOURCE_READER_READY "$pool_reader" "$reader_pid"
"$WRITER" --domain "$domain" --count 160 --rate-hz 200 --history-limit 8 --extra-samples 0 \
    --max-blocking-ms 50 --start-delay-ms 300 > "$pool_writer" 2>&1 & writer_pid=$!
wait_for RESOURCE_WRITER_MATCHED "$pool_writer" "$writer_pid"
kill -STOP "$reader_pid"
sleep 0.90
kill -CONT "$reader_pid"
wait "$writer_pid"; writer_pid=""
kill "$reader_pid" 2>/dev/null || true; wait "$reader_pid" 2>/dev/null || true; reader_pid=""

pool_oor=$(summary_value "$pool_writer" out_of_resources); pool_oor=${pool_oor:-0}
pool_timeouts=$(summary_value "$pool_writer" timeouts); pool_timeouts=${pool_timeouts:-999}
pool_errors=$(summary_value "$pool_writer" errors); pool_errors=${pool_errors:-999}
pool_max_oor_us=$(summary_value "$pool_writer" max_out_of_resources_us); pool_max_oor_us=${pool_max_oor_us:-999999999}
if [[ "$pool_oor" -gt 0 && "$pool_timeouts" -eq 0 && "$pool_errors" -eq 0 ]]; then
    record_pass hard_pool_exhaustion "out_of_resources=$pool_oor timeouts=0 extra_samples=0"
else
    record_fail hard_pool_exhaustion "out_of_resources=$pool_oor timeouts=$pool_timeouts errors=$pool_errors"
fi
if [[ "$pool_max_oor_us" -lt 10000 ]]; then
    record_pass pool_failure_is_immediate "max_out_of_resources_us=$pool_max_oor_us"
else
    record_fail pool_failure_is_immediate "max_out_of_resources_us=$pool_max_oor_us expected<10000"
fi

# ------------------------------------------------------ bounded history timeout
# Restore a one-sample reservoir. Allocation can now succeed while history is
# full, which lets add_pub_change wait for an ACK and exercise max_blocking_time.
echo
echo "== frozen matched reader: reservoir -> bounded history timeout and recovery =="
domain=193
stall_reader="$OUT/stall.reader.log"
stall_writer="$OUT/stall.writer.log"
: > "$stall_reader"; : > "$stall_writer"
"$READER" --domain "$domain" --duration-s 30 > "$stall_reader" 2>&1 & reader_pid=$!
wait_for RESOURCE_READER_READY "$stall_reader" "$reader_pid"
"$WRITER" --domain "$domain" --count 300 --rate-hz 200 --history-limit 8 --extra-samples 1 \
    --max-blocking-ms 50 --start-delay-ms 300 > "$stall_writer" 2>&1 & writer_pid=$!
wait_for RESOURCE_WRITER_MATCHED "$stall_writer" "$writer_pid"
stall_start_ms=$(date +%s%3N)
kill -STOP "$reader_pid"
sleep 1.20
resume_ms=$(date +%s%3N)
kill -CONT "$reader_pid"
wait "$writer_pid"; writer_pid=""
sleep 0.20
kill "$reader_pid" 2>/dev/null || true; wait "$reader_pid" 2>/dev/null || true; reader_pid=""

stall_timeouts=$(summary_value "$stall_writer" timeouts); stall_timeouts=${stall_timeouts:-0}
stall_oor=$(summary_value "$stall_writer" out_of_resources); stall_oor=${stall_oor:-999}
stall_errors=$(summary_value "$stall_writer" errors); stall_errors=${stall_errors:-999}
stall_max_timeout_us=$(summary_value "$stall_writer" max_timeout_us); stall_max_timeout_us=${stall_max_timeout_us:-0}
stall_recovered=$(summary_value "$stall_writer" recovered); stall_recovered=${stall_recovered:-0}
first_timeout_ms=$(grep 'WRITE_TIMEOUT' "$stall_writer" | head -1 | grep -oE 'wall_ms=[0-9]+' | cut -d= -f2 || true)
recovered_ms=$(grep 'WRITE_RECOVERED' "$stall_writer" | head -1 | grep -oE 'wall_ms=[0-9]+' | cut -d= -f2 || true)

if [[ "$stall_timeouts" -gt 0 && "$stall_oor" -eq 0 && "$stall_errors" -eq 0 ]]; then
    record_pass history_backpressure_timeout "timeouts=$stall_timeouts out_of_resources=0 extra_samples=1"
else
    record_fail history_backpressure_timeout "timeouts=$stall_timeouts out_of_resources=$stall_oor errors=$stall_errors"
fi

# 50 ms is configured max_blocking_time. Allow CI scheduling noise while rejecting
# either immediate failure or an unbounded stall.
if [[ "$stall_max_timeout_us" -ge 30000 && "$stall_max_timeout_us" -le 250000 ]]; then
    record_pass bounded_write_block "max_timeout_us=$stall_max_timeout_us configured_ms=50"
else
    record_fail bounded_write_block "max_timeout_us=$stall_max_timeout_us expected=30000..250000"
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

| scenario | history pool | max_blocking | result |
|---|---|---:|---|
| healthy matched RELIABLE reader | 8 KEEP_ALL + 1 extra | 50 ms | success=$healthy_success, timeout=$healthy_timeouts, OOR=$healthy_oor |
| frozen reader, no reservoir | 8 KEEP_ALL + 0 extra | 50 ms | OOR=$pool_oor, timeout=$pool_timeouts, max_oor_us=$pool_max_oor_us |
| frozen reader, reservoir | 8 KEEP_ALL + 1 extra | 50 ms | timeout=$stall_timeouts, OOR=$stall_oor, max_timeout_us=$stall_max_timeout_us |
| SIGCONT recovery | 8 KEEP_ALL + 1 extra | 50 ms | recovered=$stall_recovered, resume_to_recovery_ms=${recovery_ms:-n/a} |

`RETCODE_OUT_OF_RESOURCES` and `RETCODE_TIMEOUT` are intentionally separate
observations. The first means the writer could not obtain a payload/cache change;
the second means a change was allocated but could not enter full reliable history
before max_blocking_time expired.
EOF

echo
echo "resource-bound scenarios: $pass passed, $fail failed"
echo "matrix written to $OUT/matrix.md"
[[ "$fail" -eq 0 ]]
