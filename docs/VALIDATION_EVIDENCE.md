# Validation Evidence

Last updated: 2026-08-18. Claims below are tied to executed tooling, not design intent.

## Evidence generations

This repository now has two useful evidence generations:

1. **Original Milestones 1/2 local evidence** — Fast DDS 2.11.2 from Ubuntu packaging, including the first live RTPS, late-joiner, hard-death, latency and `tc netem` findings.
2. **Current regression/security evidence** — Fast DDS **2.14.6**, built from source with `SECURITY=ON`, executed in GitHub Actions on commit `c2b0c2e126e298ad2d492aef3e576ff174827072`.

The current security/regression artifact is the source of truth for Milestone 3.

## Current CI anchor

| Item | Value |
|---|---|
| Branch commit | `c2b0c2e126e298ad2d492aef3e576ff174827072` |
| GitHub Actions run | `32178914685` |
| Evidence artifact digest | `sha256:c9143f22bce757a82aa9ffd19aa17c9a548b9b9aee5f269fff0be459be574773` |
| Runner | GitHub Actions `ubuntu-24.04` |
| Scenario container | `ubuntu:22.04` |
| Compiler in scenario image | GCC 11.4.0, C++17 |
| DDS | Fast DDS **2.14.6**, source build, `SECURITY=ON` |
| IDL codegen | Fast DDS-Gen **3.3.2** |

The scenario Dockerfile uses repository-root context:

```bash
docker build -t rdtf-build -f docker/Dockerfile .
```

## Core compiler/test gates

Core CI on the current branch: **PASS**.

The core uses strict warnings and the unit suite validates sequence/freshness/schema logic, QoS-profile validation and health-state transitions.

Sanitizer CI on the current branch: **PASS** using AddressSanitizer + UndefinedBehaviorSanitizer for the core/unit-test path.

Scope limit: live DDS transport threads are not yet covered by the sanitizer gate.

## Milestone 1 — live DDS failure scenarios

The current Fast DDS 2.14.6 workflow reruns the real-process RTPS failure suite before security validation. It passed on the Milestone 3 commit.

The scenario set covers:

- healthy baseline with a positive sample-count control;
- negative control proving deadline/liveliness/QoS markers are absent while the publisher is intentionally healthy;
- sequence gap;
- duplicate;
- stale sample;
- schema mismatch;
- deadline miss;
- late join with transient-local replay;
- hard writer death/liveliness expiry;
- incompatible QoS.

### Findings retained from Milestone 1

**Graceful exit is not the same failure as hard death.** A clean writer exit can unmatch without a liveliness-loss callback. A hard death can leave the lease to expire into liveliness loss. Health logic that watches only one signal is incomplete.

**Delivered history can be unsafe history.** A transient-local late join can successfully deliver samples that immediately violate the application's freshness budget. DDS delivery success is not an application safety verdict.

## Milestone 2 — network degradation and recovery

The current 2.14.6 regression run again passed the `tc netem` matrix. One observed run from the green Milestone 3 artifact produced:

| impairment | profile | received | gaps | stale | deadline misses | p50 us | p99 us |
|---|---|---:|---:|---:|---:|---:|---:|
| baseline | RELIABLE periodic telemetry | 241 | 0 | 0 | 0 | 220 | 282 |
| 1% loss | RELIABLE periodic telemetry | 241 | 0 | 0 | 0 | 221 | 132120 |
| 15% loss | RELIABLE periodic telemetry | 162 | 2 | 65 | 10 | 173075 | 627675 |
| baseline | BEST_EFFORT high-rate sensor | 241 | 0 | 0 | 0 | 201 | 253 |
| 1% loss | BEST_EFFORT high-rate sensor | 240 | 1 | 0 | 0 | 189 | 262 |
| 15% loss | BEST_EFFORT high-rate sensor | 86 | 17 | 0 | 4 | 187 | 285 |

Do not turn one cell into a statistical guarantee. The useful engineering observation is the shape of the failure:

- RELIABLE transport can convert loss into retransmission delay/staleness;
- BEST_EFFORT can expose loss as missing application samples;
- both contracts can breach operational timing under sufficiently bad conditions.

### Partition/recovery observation from the same run

```text
partition applied          t=6995 ms
health degraded/stale      t=7241 ms
DDS liveliness lost        t=7986 ms
partition removed          t=16003 ms
health healthy again       t=16032 ms

time_to_unsafe             246 ms
time_to_lost               991 ms
recovery_to_healthy          29 ms
unsafe-before-lost window   745 ms
```

The application's freshness boundary was crossed **745 ms before** DDS declared the writer lost. Middleware health and application safety are complementary signals.

The slow-reader cases also remained destructive on a healthy network: p99 latency rose into multi-second territory and most processed samples became stale. Network availability alone is not a useful health definition.

Full Milestone 2 discussion: [NETWORK_DEGRADATION.md](NETWORK_DEGRADATION.md).

## Milestone 3 — DDS Security

Full detail: [DDS_SECURITY.md](DDS_SECURITY.md).

Result on run `32178914685`: **6 passed, 0 failed**.

```text
PASS secure_baseline
PASS untrusted_identity
PASS unauthorized_writer
PASS insecure_peer
PASS plaintext_capture_control
PASS encrypted_payload_capture
```

### Secure baseline

Trusted/authorized publisher and subscriber matched and delivered:

```text
samples_received = 300
latency_min_us    = 159
latency_p50_us    = 285
latency_p95_us    = 390
latency_p99_us    = 412
latency_p999_us   = 535
latency_max_us    = 535
latency_mean_us   = 285.883
```

These latency values are one CI run, not an SLA.

### Authentication negative

Publisher identity signed by a separate rogue identity CA:

```text
application samples received = 0
```

### Authorization negative

A publisher with a trusted identity but a signed **subscribe-only** permission was denied when creating the `SystemTelemetry` DataWriter. The publisher exited non-zero and the subscriber received zero application samples.

Fast DDS access-control evidence included:

```text
SystemTelemetry topic not found in allow rule
Problem creating associated Writer
publisher failed to start
```

### Secure vs insecure peer

A participant with governance configured to reject unauthenticated peers received:

```text
application samples from insecure publisher = 0
```

### Paired packet-capture control

Unique application marker:

```text
RDTF_WIRE_MARKER_7d91c4
```

Observed in the same successful CI artifact:

```text
plaintext pcap marker occurrences = 300
encrypted pcap marker occurrences = 0
secure samples received            = 300
```

The paired plaintext run is essential. Without it, absence from the encrypted capture could be caused by a blind/wrong-interface packet capture.

The first security run found a **harness bug**: `strings | grep -q` under `pipefail` falsely failed the plaintext positive control because `grep -q` exited early and `strings` received SIGPIPE. The pcap itself contained the marker. The corrected harness uses a binary-safe direct search and then passed 6/6. Assertions were not weakened.

Scope limit: certificate/CA identity strings may still be visible in security-handshake traffic. The test proves the application marker is not plaintext in the captured secure DDS traffic; it does not prove every handshake field is opaque.

## What remains unproven

Do not claim:

- RTI Connext implementation or interoperability;
- Fast DDS 3.x compatibility;
- security or safety certification;
- production certificate issuance, revocation or zero-downtime rotation;
- HSM-backed private keys;
- resistance to arbitrary malformed/hostile RTPS;
- tamper-evident JSONL evidence;
- statistical tail-latency guarantees;
- many-participant/CPU-contention latency;
- bounded resource-limit or writer-history-exhaustion behavior;
- large-payload behavior;
- multi-writer authority/failover;
- sanitizer cleanliness of the live DDS transport path.

These gaps drive the next experiments rather than being hidden behind a feature list.
