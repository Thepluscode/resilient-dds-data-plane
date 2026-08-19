# Resilient DDS Data Plane

A C++17 engineering lab for **reliable, diagnosable and policy-controlled real-time data distribution** in distributed and safety-conscious systems.

This is deliberately not a hello-world publisher/subscriber. It focuses on the operational failures that make real DDS systems difficult to trust:

- stale data that still arrives successfully;
- missed publication/receive deadlines;
- silent or failed writers;
- duplicate, missing and out-of-order samples;
- late-joining consumers that need recent state;
- QoS incompatibility;
- schema drift across independently deployed components;
- packet loss, jitter, reordering and partitions;
- slow consumers that turn healthy transport into unsafe state;
- competing writers and primary/standby authority failover;
- bounded writer pools and reliable-history backpressure;
- untrusted or unauthorized participants;
- weak observability when middleware says communication is "up" but application data is unhealthy.

## Problem statement

In a complex distributed system, **message delivery is not the same thing as trustworthy state**.

A consumer needs to know not only whether it received a sample, but whether the sample is:

1. fresh enough to act on;
2. the expected next version in a sequence;
3. compatible with the expected schema;
4. arriving inside its operational deadline;
5. coming from a writer that is still alive;
6. recoverable when the consumer joins late;
7. produced by the correct active authority when multiple writers exist;
8. produced without silently overrunning bounded writer resources;
9. produced by an authenticated and authorized participant;
10. observable when any of those assumptions fail.

This project provides that layer around DDS.

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

Final PR-head evidence on commit `6c986ec` produced **7/7 PASS**:

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
- many-participant/CPU-contention latency;
- multiple keyed-instance resource-limit behavior and many-reader writer pressure;
- large-payload behavior and memory-allocation determinism;
- split-brain authority behavior across a network partition or a production election/lease protocol;
- production admission-control/backpressure policy;
- sanitizer coverage of the live DDS transport path.

The gap list is part of the deliverable. A portfolio project that cannot say what it has not proven is not evidence of anything.

## Engineering thesis

> **How do you know distributed state is still safe to act on when the network, producer, authority, identity, permissions, schema, timing or resource assumptions begin to fail?**

That is the systems problem this lab is designed to make visible.
