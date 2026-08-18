# Resilient DDS Data Plane

A C++17 engineering lab for **reliable, diagnosable real-time data distribution** in distributed and safety-conscious systems.

This is deliberately not a hello-world publisher/subscriber. It focuses on the operational failures that make real DDS systems difficult to trust:

- stale data that still arrives successfully;
- missed publication/receive deadlines;
- silent or failed writers;
- duplicate, missing and out-of-order samples;
- late-joining consumers that need recent state;
- QoS incompatibility;
- schema drift across independently deployed components;
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
7. observable when any of those assumptions fail.

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
| Writer stops publishing | Deadline and liveliness health transitions |
| Consumer starts late | `TRANSIENT_LOCAL + RELIABLE` QoS profile |
| High-rate expendable sensor stream | `BEST_EFFORT + VOLATILE` profile |
| Schema drift | Explicit schema version + appendable IDL model |
| QoS mismatch | Health path for incompatible QoS callback |
| Hard-to-debug incidents | Prometheus-style counters + JSONL anomaly evidence |
| Vendor lock-in | DDS adapter boundary; core logic has no vendor dependency |

## QoS strategy

Three explicit profiles exist instead of one "magic" global configuration:

- **critical_control** — reliable, transient-local, strict 100 ms deadline, manual-by-topic liveliness.
- **periodic_telemetry** — reliable, transient-local, 250 ms deadline, deeper history.
- **high_rate_sensor** — best-effort, volatile, shallow history where the newest value matters more than retransmitting old samples.

The point is to demonstrate that QoS is an application contract, not a tuning afterthought.

## Run it

Everything builds and runs in a container. That is not a convenience: the
failure modes under test are killed processes and network namespaces.

```bash
docker build -t rdtf-build docker/
```

### The live DDS failure scenarios

Ten scenarios, each two real processes exchanging real RTPS traffic:

```bash
docker run --rm -v "$PWD":/work rdtf-build bash -c \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j && BUILD=/tmp/b bash /work/scripts/run_scenarios.sh'
```

```text
PASS baseline           <- received=130 anomalies=0
PASS negative_control   <- no deadline/liveliness/QoS events on the healthy stream
PASS packet_gap         <- kind=sequence_gap missing=1
PASS duplicate          <- kind=duplicate
PASS stale_sample       <- kind=stale
PASS schema_drift       <- kind=schema_mismatch
PASS deadline_miss      <- DEADLINE_MISSED
PASS late_joiner        <- first replayed seq=1, 25 replayed samples flagged stale
PASS dead_writer        <- LIVELINESS_LOST alive=0
PASS qos_mismatch       <- INCOMPATIBLE_QOS last_policy_id=11
```

Faults are injected by the publisher (`--drop-at`, `--duplicate-at`,
`--stale-at`, `--schema-drift-at`, `--stall-at`) or by the harness killing it.

### Network degradation and recovery

Six impairments x two QoS contracts, plus partition/recovery timing and a slow
consumer. Needs `NET_ADMIN` for `tc netem`:

```bash
docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-build bash -c \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j && BUILD=/tmp/b bash /work/scripts/run_netem.sh'
```

Full results and analysis: [docs/NETWORK_DEGRADATION.md](docs/NETWORK_DEGRADATION.md).
The headline is that under 15% packet loss the RELIABLE reader saw **0 gaps and
82 stale samples** while the BEST_EFFORT reader saw **31 gaps and 0 stale
samples** — loss becomes latency, not gaps, and the right contract depends
entirely on whether the consumer can tolerate old data or missing data.

### Across a real network

One container uses Fast DDS's shared-memory transport and never touches UDP, so
the two endpoints run in separate containers on a bridge network:

```bash
docker compose -f docker/docker-compose.yml up --abort-on-container-exit
```

### The dependency-free core alone

```bash
./scripts/build_core.sh && ./build/resilientdds_simulator
```

## DDS integration boundary

Fast DDS types live behind a pimpl in `src/dds_transport.cpp`. No other
translation unit — not the detector, not the health monitor, not the tests, not
the apps — includes a vendor header. An RTI Connext backend is a second `.cpp`
implementing the same two classes, not a rewrite.

Type support is generated from `idl/SystemTelemetry.idl` at build time by
Fast DDS-Gen. Generated code is not checked in, so the wire format cannot drift
from the model.

## What is actually built, and what is not

Built and verified — see [docs/VALIDATION_EVIDENCE.md](docs/VALIDATION_EVIDENCE.md)
for the commands and their output:

- live Fast DDS publisher and subscriber, IDL-generated types, RTPS discovery;
- QoS mapped from three named profiles onto real writer/reader QoS;
- reader callbacks wired into health: requested deadline missed, liveliness
  changed, requested incompatible QoS, sample lost, subscription matched;
- sequence-gap, duplicate, out-of-order, stale, clock-skew and schema-drift
  detection against real received samples;
- transient-local late-joiner replay, and the freshness verdict on replayed data;
- UDP-only transport by default, so `tc netem` impairments cannot be bypassed
  via shared memory;
- health state separated from health reason, with hysteresis in both directions;
- six `tc netem` impairments against both a reliable and a best-effort contract;
- partition/recovery timings, including a measured 743 ms window in which the
  consumer's state was unusable before DDS reported the writer lost;
- a slow-consumer scenario: healthy network, p99 latency over 1 s;
- measured end-to-end latency with p50/p95/p99/p99.9/max/stddev.

Not built, not claimed:

- **DDS Security** — no authentication, access control or encryption;
- **RTI Connext** — no adapter, no licence, nothing attempted;
- **Fast DDS 3.x** — the code targets the 2.11 API shipped by Ubuntu 24.04;
- repeated statistical runs (one run per impairment cell so far);
- multi-publisher, multi-instance and large-payload behaviour;
- sanitizer coverage of the DDS transport (unit-test path only);
- adversarial or malformed payload handling.

The gap list is part of the deliverable. A portfolio project that cannot say
what it has not proven is not evidence of anything.

## Why this is stronger than a portfolio toy

The project answers a systems-engineering question:

> **How do you know distributed state is still safe to act on when the network, producer, schema or timing assumptions begin to fail?**

That is the real problem this lab is designed to make visible.
