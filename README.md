# Resilient DDS Data Plane

**C++17 · DDS/RTPS · Fast DDS 2.14.6 · QoS contracts · distributed systems**

A reliability lab that answers one question with measurements rather than
assertions:

> **When the network, producer, authority, identity, schema, timing or resource
> assumptions start to fail, how do you know the distributed state you are
> holding is still safe to act on?**

Every number below is produced by a script in this repository and reproduced by
CI on every push.

## Three results worth your attention

**1. A keyed topic silently loses two thirds of its state.** Fast DDS defaults
`ResourceLimitsQosPolicy.max_instances` to **10**. Publish 32 keys and twenty-two
of them are never delivered — no error, no `on_sample_lost`, no anomaly. Every
diagnostic in this project stays green while a third of the state does not exist.

```text
default max_instances   ->  10 / 32 keys
max_instances = 64      ->  32 / 32 keys
```

**2. Packet loss does not become gaps. It becomes latency.** The same 15% loss,
two QoS contracts:

```text
RELIABLE      gaps=0    stale=59   p50=174 ms
BEST_EFFORT   gaps=32   stale=0    p50=205 us
```

Reliable recovered every sample and spent the entire freshness budget doing it.
Neither contract is "safer" — the question is whether this consumer would rather
have old data or missing data, and only the application knows.

**3. The consumer is unsafe long before DDS says anything.** Across a partition,
the freshness budget broke at **+262 ms** and DDS declared the writer lost at
**+1005 ms** — a 743 ms window in which the middleware was content and the state
was already unusable. For a 20 ms control loop that is ~37 cycles of acting on
state nothing had flagged.

The thesis in one line:

```text
MESSAGE DELIVERED  ≠  STATE SAFE TO USE
write() RETURNED TRUE  ≠  THE SAMPLE WILL ARRIVE
```

## See it yourself in 60 seconds

```bash
docker build -t rdtf-fastdds:2.14.6 -f docker/Dockerfile .
docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-fastdds:2.14.6 bash -c \
  'cmake -S . -B build-dds -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null &&
   cmake --build build-dds -j >/dev/null && BUILD=build-dds ./scripts/demo.sh'
```

Four live acts, real processes, ~60 s. `PAUSE=1` waits between them for
presenting. Narration, timings and expected questions: [docs/DEMO.md](docs/DEMO.md).

## Evidence index

| area | proven | where |
|---|---|---|
| RTPS failure semantics | 10 scenarios: gap, duplicate, stale, schema drift, deadline, liveliness, late-joiner, QoS mismatch | [VALIDATION_EVIDENCE.md](docs/VALIDATION_EVIDENCE.md) |
| Network degradation | 6 impairments × 2 QoS contracts, partition/recovery timing, time-to-unsafe | [NETWORK_DEGRADATION.md](docs/NETWORK_DEGRADATION.md) |
| DDS Security | PKI-DH auth, signed governance, rogue/unauthorized/insecure denial, paired plaintext-vs-encrypted capture | [DDS_SECURITY.md](docs/DDS_SECURITY.md) |
| Multi-writer authority | EXCLUSIVE ownership, SIGKILL failover in 86 ms, first standby sample fresh | [ROADMAP.md](docs/ROADMAP.md) |
| Writer resource bounds | `OUT_OF_RESOURCES` (8 µs) vs `TIMEOUT` (50 ms) separated; KEEP_LAST silent loss vs KEEP_ALL backpressure | [BOUNDED_RESOURCES.md](docs/BOUNDED_RESOURCES.md) |
| Fan-out isolation | 8 readers 0 gaps; frozen reader does not couple; hot key does not starve neighbours; instance cap | [FANOUT_ISOLATION.md](docs/FANOUT_ISOLATION.md) |

## How the evidence is kept honest

Green tests are cheap. These are the habits that make the numbers mean
something, and each one has caught a real defect in this repository:

- **Every suite has a positive control.** A baseline that asserts a *minimum*
  sample count, so "zero anomalies" cannot come from a run that moved no data.
- **Every suite has a negative control.** `DEADLINE_MISSED`, `LIVELINESS_LOST`
  and `INCOMPATIBLE_QOS` must be *absent* on a healthy stream, or matching them
  under fault proves nothing.
- **Guards are mutation-tested.** Each assertion is watched failing with its
  mechanism disabled before it is trusted. Two thresholds in this project passed
  on noise until that was done — a 1 ms blocking floor against ~1.2 ms of
  ordinary write latency, and a hot-key test satisfied by one sample of
  round-robin ordering. Neither was visible by reading the code.
- **Absence is not proof.** A "the marker is not in the packet capture" claim
  ships with a plaintext run proving the capture can see the marker at all.

## Architecture

```text
Producer / sensor / subsystem
          |
          v
   IDL-defined data model
          |
          v
+---------------------------+
| DDS DataWriter            |
| reliability / durability  |
| deadline / liveliness     |
| ownership / resources     |
| auth / permissions        |
+---------------------------+
          |
       DDS/RTPS
          |
          v
+---------------------------+
| DDS DataReader            |
| middleware callbacks      |
+---------------------------+
          |
          v
+---------------------------+
| Trustworthiness pipeline  |
| - sequence guard          |
| - freshness guard         |
| - schema guard            |
| - health state machine    |
+---------------------------+
       |           |
       v           v
 Prometheus     JSONL evidence
 metrics        / audit trail
```

## Pain -> mechanism

| Operational pain | Mechanism in this repo |
|---|---|
| Packet/sample loss | Per-source monotonic sequence tracking and exact gap count |
| Duplicate delivery | Duplicate sequence detection |
| Out-of-order state | Sequence regression detection |
| Stale but valid-looking data | Source timestamp freshness budget |
| Clock skew | Future-timestamp guard |
| Writer stops publishing | Deadline, match and liveliness health transitions |
| Consumer starts late | `TRANSIENT_LOCAL + RELIABLE` QoS profile |
| High-rate expendable sensor stream | `BEST_EFFORT + VOLATILE` profile |
| Schema drift | Explicit schema version + appendable IDL model |
| QoS mismatch | Health path for incompatible QoS callback |
| Network degradation | `tc netem` RELIABLE/BEST_EFFORT evidence matrix |
| Slow reader | Processing-delay scenario + freshness/latency evidence |
| Competing command writers | SHARED positive control + EXCLUSIVE ownership/strength authority probe |
| Primary writer hard failure | Measured standby takeover + first-standby freshness assertion |
| Writer allocation pool exhausted | Bounded `RESOURCE_LIMITS` probe distinguishes `RETCODE_OUT_OF_RESOURCES` |
| Reliable history cannot drain | `max_blocking_time` probe distinguishes bounded `RETCODE_TIMEOUT` and recovery |
| Rogue participant | PKI-DH authentication negative test |
| Wrongly privileged participant | Signed Access-Permissions negative test |
| Plaintext application payload | AES-GCM-GMAC protected DDS path + paired pcap control |
| Hard-to-debug incidents | Prometheus-style counters + JSONL anomaly evidence |
| Vendor lock-in | DDS adapter boundary; core logic has no vendor dependency |

## QoS strategy

Three explicit profiles exist instead of one "magic" global configuration:

- **critical_control** — reliable, transient-local, strict 100 ms deadline, manual-by-topic liveliness.
- **periodic_telemetry** — reliable, transient-local, 250 ms deadline, deeper history.
- **high_rate_sensor** — best-effort, volatile, shallow history where the newest value matters more than retransmitting old samples.

The point is to demonstrate that QoS is an application contract, not a tuning afterthought.

## Run it

Everything builds and runs in a container. That is not just convenience: the failure modes under test include killed processes, network namespaces, `tc netem`, packet capture, frozen readers and bounded writer histories.

The scenario image pins **Fast DDS 2.14.6** and builds it with `SECURITY=ON`:

```bash
docker build -t rdtf-build -f docker/Dockerfile .
```

### Live DDS failure scenarios

Ten scenarios, each using real processes exchanging RTPS traffic:

```bash
docker run --rm --network host -v "$PWD":/work rdtf-build bash -lc \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j"$(nproc)" && BUILD=/tmp/b /work/scripts/run_scenarios.sh'
```

The suite proves a healthy positive/negative control before accepting evidence for sequence gaps, duplicates, stale samples, schema drift, missed deadlines, late joining, hard writer death and incompatible QoS.

### Network degradation and recovery

Six impairments x two QoS contracts, plus partition/recovery timing and a slow consumer. Needs `NET_ADMIN` for `tc netem`:

```bash
docker run --rm --network host --cap-add=NET_ADMIN -v "$PWD":/work rdtf-build bash -lc \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j"$(nproc)" && BUILD=/tmp/b /work/scripts/run_netem.sh'
```

Full results and analysis: [docs/NETWORK_DEGRADATION.md](docs/NETWORK_DEGRADATION.md).
The important result is qualitative, not a single benchmark number: with RELIABLE QoS, transport loss can become **retransmission latency and stale state**; with BEST_EFFORT it is more likely to become **visible application gaps**. The right contract depends on what the consumer is allowed to tolerate.

### DDS Security validation

Generate disposable test credentials and run the six-case security matrix:

```bash
./scripts/security/generate_test_pki.sh /tmp/rdtf-security-pki
BUILD=/tmp/b PKI=/tmp/rdtf-security-pki OUT=/tmp/security-evidence \
  ./scripts/security/run_security_scenarios.sh
```

The current regression chain preserves **6/6 PASS**:

```text
PASS secure_baseline
PASS untrusted_identity
PASS unauthorized_writer
PASS insecure_peer
PASS plaintext_capture_control
PASS encrypted_payload_capture
```

Observed controls:

- trusted/authorized secure path: **300 samples received**;
- trusted identity without publish permission: writer creation rejected;
- rogue identity CA: **0 application samples**;
- insecure peer against secure governance: **0 application samples**;
- plaintext pcap: unique application marker visible **300 times**;
- encrypted pcap: the same application marker visible **0 times** while all 300 secure samples were received.

The last point is deliberately narrow: it proves the application marker is not present in plaintext in the captured secure DDS traffic. It does **not** mean every security-handshake field is opaque; certificate/CA strings can still be observable during authentication.

More detail: [docs/DDS_SECURITY.md](docs/DDS_SECURITY.md).

### Multi-writer authority and hard failover

The authority harness uses a SHARED-ownership positive control first so writer identity instrumentation must prove it can see both competing writers. It then switches to EXCLUSIVE ownership with primary strength 100 and standby strength 10:

```bash
BUILD=/tmp/b OUT=/tmp/authority-evidence ./scripts/run_authority_scenarios.sh
```

On the verified main-line Milestone 4A run, both writers were alive and matched, the lower-strength standby remained invisible while the primary was healthy, and a hard primary death caused the standby to become visible in **87 ms**. The first visible standby sample had **0 ms measured age** against the test's freshness budget.

That assertion is intentional: correct ownership with stale takeover state is still unsafe application state.

### Writer resource bounds and backpressure

The writer-side harness freezes the entire matched RELIABLE reader process with `SIGSTOP`, so DDS protocol threads stop acknowledging data while the writer obligation remains established:

```bash
BUILD=/tmp/b OUT=/tmp/resource-evidence ./scripts/run_resource_bounds.sh
```

The experiment deliberately separates two failure paths rather than collapsing them into "publish failed":

- with `KEEP_ALL max_samples=8` and **no extra reservoir**, the writer hits fast `RETCODE_OUT_OF_RESOURCES` when it cannot allocate another cache change;
- with `extra_samples=1`, allocation can succeed while history is full, so the writer reaches the bounded reliable-history wait and returns `RETCODE_TIMEOUT` near its configured 50 ms `max_blocking_time`;
- after `SIGCONT`, successful publication must recover.

The immutable Milestone 4B implementation evidence anchor is commit `6c986ec`, validated by combined run `32227232142` and artifact SHA-256 `8a3208f19750df5a4542832f4e6169c0bfc317c79c36975672caa0a4d2fcd605`. Later documentation-only successors are required to pass the same CI gates but do not redefine this experiment.

That implementation run produced **7/7 PASS**:

```text
healthy_control                 success=200 timeouts=0 out_of_resources=0 errors=0
hard_pool_exhaustion            out_of_resources=124 timeouts=0
pool_failure_is_immediate       max_out_of_resources_us=8
history_backpressure_timeout    timeouts=16 out_of_resources=0
bounded_write_block             max_timeout_us=50126 (configured 50 ms)
timeout_causality               PASS
writer_recovery                 resume_to_write_recovery_ms=6
```

The first version of this experiment was intentionally retained in the PR history because it falsified an assumption: `extra_samples=0` caused allocation-pool exhaustion even in the supposed healthy control. That failure exposed the distinction between allocation headroom and reliable-history backpressure and led to the A/B design above.

### Across a real network namespace boundary

Publisher and subscriber run in separate containers on a bridge network. The applications default to UDP-only transport, so shared memory cannot silently bypass the network path:

```bash
docker compose -f docker/docker-compose.yml up --abort-on-container-exit
```

### Dependency-free core alone

```bash
./scripts/build_core.sh && ./build/resilientdds_simulator
```

## DDS integration boundary

Fast DDS types live behind a pimpl in `src/dds_transport.cpp`. No other translation unit — not the detector, not the health monitor, not the tests, not the apps — needs Fast DDS headers. An RTI Connext backend is intended to implement the same application-facing seam rather than force a rewrite of the trustworthiness layer.

The Milestone 4A/4B probes intentionally use generated Fast DDS types directly. They exist to falsify ownership and resource semantics before those vendor-specific concepts are promoted into the reusable adapter API.

Type support is generated from `idl/SystemTelemetry.idl` at build time by Fast DDS-Gen. Generated code is not checked in, so the wire model is derived from the IDL source of truth.

## What is actually built and verified

See [docs/VALIDATION_EVIDENCE.md](docs/VALIDATION_EVIDENCE.md) for the evidence boundary.

- live Fast DDS publisher and subscriber with IDL-generated types and RTPS discovery;
- Fast DDS **2.14.6** regression/security image built from source with `SECURITY=ON`;
- QoS mapped from three named profiles onto real writer/reader QoS;
- reader callbacks wired into health: deadline, liveliness, incompatible QoS, sample loss and matching;
- sequence-gap, duplicate, out-of-order, stale, clock-skew and schema-drift detection against received samples;
- transient-local late-joiner replay, including freshness verdicts on replayed history;
- UDP-only transport by default for impairment tests;
- health state separated from health reason, with hysteresis;
- six `tc netem` impairments against RELIABLE and BEST_EFFORT contracts;
- partition/recovery timing and time-to-unsafe-state measurement;
- slow-consumer scenario showing that healthy networking does not imply healthy application state;
- latency percentiles p50/p95/p99/p99.9/max/stddev;
- DDS Security PKI-DH authentication, signed governance/permissions and AES-GCM-GMAC configuration;
- negative security tests for rogue identity, unauthorized writer and insecure peer;
- paired plaintext/encrypted packet-capture controls;
- SHARED positive control and EXCLUSIVE ownership-strength primary/standby failover;
- hard-failure takeover timing with first-standby freshness assertion;
- writer-side bounded resource experiments distinguishing immediate allocation exhaustion from bounded reliable-history timeout;
- writer recovery after a frozen matched reader resumes.

## Still not built or not proven

Do not claim these:

- **RTI Connext** — no adapter, SDK validation or interoperability run yet;
- **Fast DDS 3.x** compatibility — the supported regression/security baseline is 2.14.6;
- production certificate rotation, revocation, HSM-backed keys or zero-downtime credential rollover;
- security or safety certification;
- resistance to arbitrary malformed/hostile RTPS traffic;
- tamper-evident JSONL evidence;
- statistical latency-tail claims — impairment cells are not yet repeated enough for that;
- many-participant/CPU-contention latency beyond 8 readers and 32 instances;
- fan-out combined with `tc netem` impairment;
- reader-side RSS/CPU, and soak-duration churn rather than 5 cycles;
- large-payload behavior and memory-allocation determinism;
- split-brain authority behavior across a network partition or a production election/lease protocol;
- production admission-control/backpressure policy;
- sanitizer coverage of the live DDS transport path.

The gap list is part of the deliverable. A portfolio project that cannot say what it has not proven is not evidence of anything.

## Engineering thesis

> **How do you know distributed state is still safe to act on when the network, producer, authority, identity, permissions, schema, timing or resource assumptions begin to fail?**

That is the systems problem this lab is designed to make visible.
