# Roadmap

## Milestone 0 — Core semantics (implemented)
- IDL domain model;
- sequence/freshness/schema anomaly detection;
- health state machine;
- QoS contracts;
- metrics;
- JSONL evidence;
- deterministic simulator;
- unit tests.

## Milestone 1 — Live Fast DDS path (implemented)
- type support generated from IDL at build time by Fast DDS-Gen;
- live publisher and subscriber as separate processes over RTPS;
- requested-deadline, liveliness-changed, incompatible-QoS, sample-lost and
  subscription-matched callbacks mapped into HealthMonitor and metrics;
- late-joiner test, with the finding that transient-local replay delivers data
  that is already stale;
- dead-writer test, with the finding that only an ungraceful death expires the
  lease — a clean exit unmatches instead;
- two-container bridge-network run, so the transport is UDP rather than shared
  memory;
- ten-scenario harness with a positive and a negative control, output in
  `docs/VALIDATION_EVIDENCE.md`.

Built against Fast DDS 2.11 (`libfastrtps-dev`, Ubuntu 24.04). Fast DDS 3.x is
untested.

## Milestone 2 — Network degradation and recovery (implemented)
Evidence: [NETWORK_DEGRADATION.md](NETWORK_DEGRADATION.md).

- UDP-only transport enforced, so impairments cannot be bypassed by shared memory;
- `tc netem` matrix: loss 1%/15%, delay 50 ms, jitter 50+/-50 ms, reorder 20%,
  each against RELIABLE and BEST_EFFORT;
- partition/recovery timings anchored on a subscriber-emitted clock epoch;
- time-to-unsafe vs time-to-lost, showing the unsafe window before DDS reacts;
- health reasons and bidirectional hysteresis, with a mutation-tested regression
  guard against stale retransmissions counting as recovery;
- slow-consumer scenario on a healthy network;
- latency percentiles p50/p95/p99/p99.9/max/stddev.

Outstanding:
- repeated runs for statistical claims about the tail;
- CPU contention and many-participant latency;
- bounded resource-limit and writer-history-exhaustion tests;
- large payloads, multiple publishers, multiple keyed instances;
- soak test;
- sanitizers over the DDS transport path.

## Milestone 3 — Security
- DDS Security participant authentication;
- governance/permissions files;
- unauthorized writer negative test;
- certificate rotation runbook;
- signed or hash-chained diagnostic evidence if the use case requires it.

## Milestone 4 — Vendor portability
- RTI Connext adapter;
- common semantic test suite against both backends;
- document QoS mapping differences and unsupported behavior.

## Milestone 5 — Systems-integration showcase
- three simulated subsystems;
- one command/control topic and two telemetry classes;
- health dashboard/export;
- failure-injection demo script;
- five-minute interview walkthrough.
