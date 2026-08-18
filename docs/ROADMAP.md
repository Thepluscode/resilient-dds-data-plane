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
- deadline, liveliness, incompatible-QoS, sample-lost and subscription-matched callbacks mapped into health/metrics;
- late-joiner test, including the finding that transient-local replay can deliver already-stale state;
- dead-writer test, including the finding that graceful exit unmatches while hard death can expire liveliness;
- two-container bridge-network validation;
- ten-scenario harness with positive and negative controls.

The original Milestone 1 evidence used Fast DDS 2.11 from Ubuntu packaging. The current regression/security image upgrades the active validation baseline to **Fast DDS 2.14.6** built from source.

## Milestone 2 — Network degradation and recovery (implemented)
Evidence: [NETWORK_DEGRADATION.md](NETWORK_DEGRADATION.md).

- UDP-only transport enforced, so impairments cannot be bypassed by shared memory;
- `tc netem` matrix: loss 1%/15%, delay 50 ms, jitter, reorder 20%, each against RELIABLE and BEST_EFFORT;
- partition/recovery timings anchored on the subscriber clock;
- time-to-unsafe vs time-to-lost;
- health reasons and bidirectional hysteresis;
- mutation-tested guard against stale retransmissions counting as recovery;
- slow-consumer scenario on a healthy network;
- latency percentiles p50/p95/p99/p99.9/max/stddev.

## Milestone 3 — DDS Security (implemented and validated)
Evidence: [DDS_SECURITY.md](DDS_SECURITY.md).

- Fast DDS 2.14.6 built with `SECURITY=ON`;
- PKI-DH participant authentication;
- signed governance and role permissions;
- Access-Permissions authorization;
- AES-GCM-GMAC protected secure path;
- disposable runtime PKI, no committed private credentials;
- rogue-identity negative test;
- trusted-but-unauthorized writer negative test;
- insecure-peer negative test;
- paired plaintext/encrypted pcap controls;
- full security matrix: **6/6 PASS** on GitHub Actions run `32178914685`.

Outstanding security hardening:
- certificate expiry/revocation and zero-downtime rotation;
- hostile/malformed RTPS and resource-exhaustion testing;
- dependency/CVE release gate;
- tamper-evident evidence only where the target system requires it.

## Milestone 4 — Multi-writer authority and bounded resources
This is the next engineering priority because it addresses a real command/control pain: more than one writer may be alive, but only the correct authority should control a keyed state.

Planned evidence:
- two writers for the same keyed instance;
- explicit primary/standby authority contract;
- deterministic ownership/failover behavior;
- hard-kill primary and measure standby takeover gap;
- prove no split-brain application state during overlap;
- multiple independent keyed sources with per-source sequence/freshness isolation;
- bounded reader/writer history and resource-limit behavior;
- writer-history exhaustion/backpressure test;
- many-participant discovery/latency run;
- soak run with fixed memory/CPU evidence.

## Milestone 5 — Vendor portability
- RTI Connext adapter when an SDK/licence is available;
- common semantic test suite against both backends;
- document QoS/security mapping differences and unsupported behavior;
- no RTI experience claim until that backend compiles and passes the shared suite.

## Milestone 6 — Systems-integration showcase
- three simulated subsystems;
- one command/control topic and two telemetry classes;
- authority/failover view;
- health/security dashboard or export;
- failure-injection demo script;
- five-minute interview walkthrough showing evidence rather than feature slides.

## Current evidence gaps

Still deliberately unclaimed:
- Fast DDS 3.x compatibility;
- RTI Connext;
- statistical latency-tail guarantees;
- many-participant/CPU-contention latency;
- bounded resource exhaustion;
- large payload behavior;
- multi-writer authority/failover;
- live DDS transport sanitizer coverage;
- security/safety certification.
