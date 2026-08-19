# Validation Evidence

Last updated: 2026-08-19. Claims below are tied to executed tooling, not design intent.

## Evidence generations

This repository has four useful evidence generations:

1. **Milestones 1/2 local evidence** — the first live RTPS, late-joiner, hard-death, latency and `tc netem` findings.
2. **Milestone 3 security evidence** — Fast DDS **2.14.6** built from source with `SECURITY=ON`, proving authentication, access control and protected application traffic with paired packet-capture controls.
3. **Milestone 4A authority evidence** — merged on `main`, proving SHARED writer visibility plus EXCLUSIVE primary/standby ownership-strength behavior and hard-failure takeover.
4. **Milestone 4B writer-resource evidence** — current PR candidate, distinguishing immediate allocation-pool exhaustion from bounded RELIABLE history backpressure and proving post-stall writer recovery.

The latest combined workflow reruns the earlier RTPS, network, security and authority suites before accepting Milestone 4B evidence.

## Evidence anchors

| Item | Value |
|---|---|
| Verified main baseline before Milestone 4B | `4ad12c28591baf64fffc803c851342403f92477d` |
| Immutable Milestone 4B implementation head | `6c986ecf469c4faaac9fcf9f64135d0c406161a6` |
| Milestone 4B implementation workflow | `32227232142` |
| Implementation evidence artifact | `9356081922` |
| Implementation artifact SHA-256 | `8a3208f19750df5a4542832f4e6169c0bfc317c79c36975672caa0a4d2fcd605` |
| Runner | GitHub Actions `ubuntu-24.04` |
| Scenario container | `ubuntu:22.04` |
| Compiler in scenario image | GCC 11.4.0, C++17 |
| DDS | Fast DDS **2.14.6**, source build, `SECURITY=ON` |
| IDL codegen | Fast DDS-Gen **3.3.2** |

The implementation anchor is intentionally immutable. Documentation-only commits after `6c986ec` must still pass normal CI, sanitizers and the full combined scenario workflow, but they do not redefine the measured Milestone 4B experiment or create a self-referential "current docs SHA" evidence loop.

The scenario Dockerfile uses repository-root context:

```bash
docker build -t rdtf-build -f docker/Dockerfile .
```

## Core compiler/test gates

On the Milestone 4B implementation head:

- core CI: **PASS**;
- AddressSanitizer + UndefinedBehaviorSanitizer core/unit-test gate: **PASS**;
- full Fast DDS RTPS/netem/security/authority/resource-bound scenario workflow: **PASS**.

Later documentation-only successors are also required to pass these gates before review/merge.

The core uses strict warnings and the unit suite validates sequence/freshness/schema logic, QoS-profile validation and health-state transitions.

Scope limit: live DDS transport threads are not yet covered by the sanitizer gate.

## Milestone 1 — live DDS failure scenarios

The current Fast DDS 2.14.6 workflow reruns the real-process RTPS failure suite before every later milestone gate.

Result on the Milestone 4B implementation run: **10 passed, 0 failed**.

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

Implementation-run controls included:

```text
PASS baseline          received=137 anomalies=0
PASS negative_control no deadline/liveliness/QoS events on healthy stream
PASS dead_writer       LIVELINESS_LOST alive=0
PASS qos_mismatch      INCOMPATIBLE_QOS last_policy_id=11
```

### Findings retained from Milestone 1

**Graceful exit is not the same failure as hard death.** A clean writer exit can unmatch without a liveliness-loss callback. A hard death can leave the lease to expire into liveliness loss. Health logic that watches only one signal is incomplete.

**Delivered history can be unsafe history.** A transient-local late join can successfully deliver samples that immediately violate the application's freshness budget. DDS delivery success is not an application safety verdict.

## Milestone 2 — network degradation and recovery

The Milestone 4B implementation run passed the `tc netem` matrix: **14 runs, 0 failed**.

One implementation-run slice:

| impairment | profile | received | gaps | stale | deadline misses | p50 us | p99 us |
|---|---|---:|---:|---:|---:|---:|---:|
| baseline | RELIABLE periodic telemetry | 241 | 0 | 0 | 0 | 145 | 247 |
| 1% loss | RELIABLE periodic telemetry | 241 | 0 | 11 | 1 | 142 | 414597 |
| 15% loss | RELIABLE periodic telemetry | 238 | 3 | 88 | 10 | 131884 | 625594 |
| baseline | BEST_EFFORT high-rate sensor | 241 | 0 | 0 | 0 | 128 | 179 |
| 1% loss | BEST_EFFORT high-rate sensor | 240 | 1 | 0 | 0 | 120 | 179 |
| 15% loss | BEST_EFFORT high-rate sensor | 203 | 34 | 0 | 4 | 125 | 169 |

Do not turn one cell into a statistical guarantee. The useful engineering observation is the shape of the failure:

- RELIABLE transport can convert loss into retransmission delay/staleness;
- BEST_EFFORT can expose loss as missing application samples;
- both contracts can breach operational timing under sufficiently bad conditions.

### Partition/recovery observation from the implementation run

```text
partition applied          t=6995 ms
health degraded/stale      t=7234 ms
DDS liveliness lost        t=7998 ms
partition removed          t=16003 ms
health healthy again       t=16037 ms

time_to_unsafe             239 ms
time_to_lost               1003 ms
recovery_to_healthy          34 ms
unsafe-before-lost window   764 ms
```

The application's freshness boundary was crossed **764 ms before** DDS declared the writer lost. Middleware health and application safety are complementary signals.

The slow-reader cases again produced multi-second tail latency and mostly stale processed samples despite a healthy network. Network availability alone is not a useful health definition.

Full Milestone 2 discussion: [NETWORK_DEGRADATION.md](NETWORK_DEGRADATION.md).

## Milestone 3 — DDS Security

Full detail: [DDS_SECURITY.md](DDS_SECURITY.md).

Result on the Milestone 4B combined implementation run: **6 passed, 0 failed**.

```text
PASS secure_baseline              received=300
PASS untrusted_identity           received=0
PASS unauthorized_writer          received=0
PASS insecure_peer                received=0
PASS plaintext_capture_control    received=300
PASS encrypted_payload_capture    received=300
```

### Security controls

The access-control negative uses a trusted identity with signed subscribe-only permissions; local DataWriter creation is rejected and the remote consumer receives zero application samples.

The rogue-identity negative uses a certificate chain rooted in a different identity CA; zero application samples are accepted.

The secure-vs-insecure negative confirms a secure participant governed to reject unauthenticated peers does not accept the insecure publisher's application data.

### Paired packet-capture control

Unique application marker:

```text
RDTF_WIRE_MARKER_7d91c4
```

The validated security generation proved:

```text
plaintext pcap marker occurrences = 300
encrypted pcap marker occurrences = 0
secure samples received            = 300
```

The paired plaintext run is essential. Without it, absence from the encrypted capture could be caused by a blind/wrong-interface packet capture.

Scope limit: certificate/CA identity strings may still be visible in security-handshake traffic. The test proves the application marker is not plaintext in the captured secure DDS traffic; it does not prove every handshake field is opaque.

## Milestone 4A — multi-writer authority and failover

Milestone 4A is merged into the verified `main` baseline and is rerun by the Milestone 4B combined workflow.

The harness starts with a **SHARED ownership positive control**. Two writers publish the same keyed instance, and the reader uses DDS publication identity to prove it sees both distinct writers. Only after this control succeeds does the experiment evaluate EXCLUSIVE ownership.

The EXCLUSIVE case runs:

```text
primary ownership strength = 100
standby ownership strength = 10
```

Both writers remain alive and matched. The lower-strength standby keeps writing but must remain invisible to the reader while the primary is healthy. The primary is then terminated with `SIGKILL` and the standby must become visible while its first sample is still inside the freshness budget.

Milestone 4B implementation-run evidence retained 4A:

```text
PASS shared_control
  handles=2
  owner_changes=380

PASS exclusive_primary_dominates
  standby_wrote=40 while invisible

PASS exclusive_hard_failover
  failover_ms=87
  first_standby_age_ms=0
```

This proves the tested Fast DDS ownership-strength behavior. It does **not** prove a distributed election protocol, split-brain prevention across partitions, or RTI Connext behavior.

## Milestone 4B — writer resource bounds and recovery

Milestone 4B addresses a different overload layer from the existing slow-reader test. Sleeping in application receive processing can make consumer data stale while DDS protocol threads still acknowledge reliable traffic. The 4B harness therefore freezes the **entire matched reader process with `SIGSTOP`**, stopping protocol progress and ACKs while the writer remains obligated to that reader.

### First hypothesis — falsified

The initial probe configured:

```text
KEEP_ALL
max_samples=8
extra_samples=0
max_blocking_time=50 ms
```

It expected a healthy reader to drain the writer fast enough that all writes would succeed. That assumption was wrong. The first run produced:

```text
attempts=200
success=178
timeouts=0
out_of_resources=22
```

The failures were only a few microseconds long, showing hard allocation pressure rather than a 50 ms history wait.

Inspection of the Fast DDS 2.14.6 writer path established two distinct outcomes that the corrected probe now measures separately:

1. a payload/cache change cannot be obtained -> `RETCODE_OUT_OF_RESOURCES`;
2. a change can be allocated, but cannot enter full reliable history before `max_blocking_time` -> `RETCODE_TIMEOUT`.

`extra_samples` provides the small reservoir needed to let the second path be reached when history itself is full.

### Corrected A/B matrix

Immutable implementation evidence, run `32227232142`:

```text
PASS healthy_control
  success=200
  timeouts=0
  out_of_resources=0
  errors=0

PASS hard_pool_exhaustion
  out_of_resources=124
  timeouts=0
  extra_samples=0

PASS pool_failure_is_immediate
  max_out_of_resources_us=8

PASS history_backpressure_timeout
  timeouts=16
  out_of_resources=0
  extra_samples=1

PASS bounded_write_block
  max_timeout_us=50126
  configured max_blocking_time=50 ms

PASS timeout_causality
  first timeout occurred inside the SIGSTOP interval

PASS writer_recovery
  resume_to_write_recovery_ms=6
```

Result: **7 passed, 0 failed**.

The engineering distinction is the important result: a bounded reliable writer can fail immediately because its allocation pool has no headroom, or it can block for a bounded interval waiting for reliable history to drain and then time out. Those should not be collapsed into one generic publish-failure metric or one recovery policy.

## Current evidence boundary

What is now proven in the Fast DDS 2.14.6 test environment:

- real RTPS publisher/subscriber behavior and QoS callback semantics;
- stale/gap/duplicate/schema/deadline/liveliness detection;
- transient-local late-join replay with application freshness verdicts;
- UDP-only network impairment behavior under `tc netem`;
- partition failure detection and recovery timing;
- reader-side overload/freshness failure;
- DDS Security authentication/access-control/protected-traffic controls;
- SHARED and EXCLUSIVE multi-writer authority behavior with hard primary failover;
- writer-side allocation-pool exhaustion;
- writer-side bounded reliable-history timeout;
- successful writer recovery after the matched reader resumes.

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
- multiple keyed-instance resource-limit behavior;
- many-reader writer-pressure behavior;
- large-payload behavior;
- deterministic memory-allocation guarantees;
- split-brain authority behavior across network partitions;
- a production authority/election/lease protocol;
- a production admission-control/backpressure policy;
- sanitizer cleanliness of the live DDS transport path.

These gaps drive the next experiments rather than being hidden behind a feature list.
