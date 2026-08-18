#!/usr/bin/env bash
# Milestone 3: prove DDS Security behavior, including controls that can fail the
# test if the harness is not actually observing the wire.
set -euo pipefail

BUILD=${BUILD:-/tmp/b-security}
PKI=${PKI:-/tmp/rdtf-security-pki}
OUT=${OUT:-/tmp/security-evidence}
PUB="$BUILD/resilientdds_dds_publisher"
SUB="$BUILD/resilientdds_dds_subscriber"
mkdir -p "$OUT"

if [[ ! -x "$PUB" || ! -x "$SUB" ]]; then
    echo "DDS binaries missing under $BUILD" >&2
    exit 2
fi
if [[ ! -f "$PKI/main/governance.smime" ]]; then
    echo "PKI missing under $PKI" >&2
    exit 2
fi

metric() {
    awk -v metric="$2" '$1 == metric { print $2; exit }' "$1" 2>/dev/null
}
assert_zero_received() {
    local prom=$1 label=$2
    local recv
    recv=$(metric "$prom" rdtf_samples_received_total || true); recv=${recv:-0}
    if [[ ${recv%.*} -ne 0 ]]; then
        echo "FAIL $label: received=$recv (expected zero)" >&2
        return 1
    fi
    echo "PASS $label received=0"
}
assert_received() {
    local prom=$1 label=$2
    local recv
    recv=$(metric "$prom" rdtf_samples_received_total || true); recv=${recv:-0}
    if [[ ${recv%.*} -le 0 ]]; then
        echo "FAIL $label: no data received" >&2
        return 1
    fi
    echo "PASS $label received=$recv"
}

run_sub() {
    local domain=$1 out=$2 secdir=${3:-} role=${4:-subscriber} source=${5:-radar-01}
    local args=(--domain "$domain" --duration-s 6 --source-id "$source"
                --metrics-out "$out.prom" --audit-out "$out.jsonl")
    if [[ -n "$secdir" ]]; then args+=(--security-dir "$secdir" --security-role "$role"); fi
    "$SUB" "${args[@]}" > "$out.sub.log" 2>&1 &
    SUB_PID=$!
}
run_pub() {
    local domain=$1 out=$2 secdir=${3:-} role=${4:-publisher} source=${5:-radar-01}
    local args=(--domain "$domain" --count 300 --rate-hz 80 --source-id "$source" --wait-match-ms 2500)
    if [[ -n "$secdir" ]]; then args+=(--security-dir "$secdir" --security-role "$role"); fi
    set +e
    "$PUB" "${args[@]}" > "$out.pub.log" 2>&1
    PUB_RC=$?
    set -e
}

pass=0
fail=0
record() {
    local label=$1 status=$2 detail=$3
    printf '| %s | %s | %s |\n' "$label" "$status" "$detail" >> "$OUT/security-matrix.md"
    if [[ "$status" == PASS ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
}

cat > "$OUT/security-matrix.md" <<'MD'
# DDS Security scenario matrix

| scenario | result | evidence |
|---|---|---|
MD

# 1) Secure happy path: both identities trusted, permissions authorize the topic.
domain=141; base="$OUT/secure_baseline"
run_sub "$domain" "$base" "$PKI/main" subscriber
sleep 1
run_pub "$domain" "$base" "$PKI/main" publisher
wait "$SUB_PID"
if assert_received "$base.prom" secure_baseline && grep -q 'MATCH writers=1' "$base.sub.log"; then
    record secure_baseline PASS "trusted publisher/subscriber matched and moved data"
else
    record secure_baseline FAIL "secure peers did not exchange data"
fi

# 2) Authentication negative: publisher uses a certificate signed by a different
# identity CA. The trusted subscriber must never consume its samples.
domain=142; base="$OUT/untrusted_identity"
run_sub "$domain" "$base" "$PKI/main" subscriber
sleep 1
run_pub "$domain" "$base" "$PKI/untrusted" untrusted-publisher
wait "$SUB_PID"
if assert_zero_received "$base.prom" untrusted_identity; then
    record untrusted_identity PASS "rogue identity CA rejected; zero application samples"
else
    record untrusted_identity FAIL "untrusted identity reached application data path"
fi

# 3) Authorization negative: the identity is trusted, but its signed permissions
# grant subscribe only. A publisher DataWriter must not become usable.
domain=143; base="$OUT/unauthorized_writer"
run_sub "$domain" "$base" "$PKI/main" subscriber
sleep 1
run_pub "$domain" "$base" "$PKI/main" unauthorized-publisher
wait "$SUB_PID"
if assert_zero_received "$base.prom" unauthorized_writer && [[ "$PUB_RC" -ne 0 ]]; then
    record unauthorized_writer PASS "trusted identity denied local writer creation (publisher rc=$PUB_RC)"
else
    record unauthorized_writer FAIL "writer was not decisively denied (publisher rc=$PUB_RC)"
fi

# 4) Secure-vs-insecure negative: governance forbids unauthenticated participants.
domain=144; base="$OUT/insecure_peer"
run_sub "$domain" "$base" "$PKI/main" subscriber
sleep 1
run_pub "$domain" "$base"
wait "$SUB_PID"
if assert_zero_received "$base.prom" insecure_peer; then
    record insecure_peer PASS "secure participant did not accept an insecure peer"
else
    record insecure_peer FAIL "unauthenticated peer reached application data path"
fi

# Wire-control pair. A plaintext run must expose a unique payload marker in the
# pcap; otherwise the encrypted test below would be meaningless because the
# capture path itself could be blind.
if [[ $(id -u) -ne 0 ]]; then
    echo "wire proof requires root (tcpdump); run this harness in the supplied container" >&2
    exit 2
fi
capture_run() {
    local label=$1 domain=$2 secure=$3 marker=$4
    local base="$OUT/$label"
    rm -f "$base.pcap"
    tcpdump -i any -U -w "$base.pcap" udp >/dev/null 2>&1 &
    local cap=$!
    sleep 1
    if [[ "$secure" == yes ]]; then
        run_sub "$domain" "$base" "$PKI/main" subscriber "$marker"
        sleep 1
        run_pub "$domain" "$base" "$PKI/main" publisher "$marker"
    else
        run_sub "$domain" "$base" "" subscriber "$marker"
        sleep 1
        run_pub "$domain" "$base" "" publisher "$marker"
    fi
    wait "$SUB_PID"
    kill -INT "$cap" 2>/dev/null || true
    wait "$cap" 2>/dev/null || true
}

marker="RDTF_WIRE_MARKER_7d91c4"
capture_run plaintext_capture_control 145 no "$marker"
# Search the binary capture directly. Using `strings | grep -q` under pipefail is
# incorrect: grep exits on the first match, strings gets SIGPIPE, and a valid
# positive control is reported as failed.
if assert_received "$OUT/plaintext_capture_control.prom" plaintext_capture_control && \
   grep -aFq "$marker" "$OUT/plaintext_capture_control.pcap"; then
    record plaintext_capture_control PASS "pcap control sees plaintext application marker"
else
    record plaintext_capture_control FAIL "capture control could not see expected plaintext marker"
fi

capture_run encrypted_payload_capture 146 yes "$marker"
if assert_received "$OUT/encrypted_payload_capture.prom" encrypted_payload_capture && \
   ! grep -aFq "$marker" "$OUT/encrypted_payload_capture.pcap"; then
    record encrypted_payload_capture PASS "secure peers moved data; plaintext marker absent from pcap"
else
    record encrypted_payload_capture FAIL "wire marker remained visible or secure data did not move"
fi

printf '\nsecurity scenarios: %d passed, %d failed\n' "$pass" "$fail"
cat "$OUT/security-matrix.md"
[[ $fail -eq 0 ]]
