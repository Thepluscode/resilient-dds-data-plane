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

## Milestone 4A — Multi-writer authority (implemented)
Evidence: PR #2, `scripts/run_authority_scenarios.sh`.

- two writers on the same keyed instance, SHARED control proving both are visible;
- EXCLUSIVE primary (strength 100) suppresses standby (strength 10) while alive;
- SIGKILL of the primary produces takeover in 86 ms;
- the first standby sample is inside the 250 ms freshness budget, so authority is
  correct AND the state is usable;
- a control asserting the standby was actually publishing while suppressed,
  because a dead standby is equally invisible.

## Milestone 4B — Writer-side bounded resources (implemented)
Evidence: [BOUNDED_RESOURCES.md](BOUNDED_RESOURCES.md).

- QoS contract extended with history kind, `max_samples` and `max_blocking_ms`;
- KEEP_LAST vs KEEP_ALL under a reader permanently slower than its writer;
- measured: KEEP_LAST loses data with zero write failures, while KEEP_ALL turns
  the same loss into blocking and failed writes the producer can act on;
- `max_blocking_time = 0` makes the producer learn sooner and lose more;
- validate() refuses unbounded keep-all reliable history, with a unit test
  asserting the rule does not over-fire on best-effort;
- four controls, each observed failing under mutation;
- write failures split into `TIMEOUT` (history backpressure) and
  `OUT_OF_RESOURCES` (allocation exhaustion), which a single failure counter
  conflates. This harness produces only the former because `max_samples` also
  reserves allocation; `scripts/run_resource_bounds.sh` starves allocation and
  produces the latter.

Outstanding for Milestone 4:
- direct writer memory/RSS measurement rather than a configured ceiling;
- multiple independent keyed sources with per-source sequence/freshness isolation;
- no-split-brain assertion during writer overlap;
- bounded resources combined with `tc netem` impairment;
- many-participant discovery/latency run;
- soak run with fixed memory/CPU evidence.

## Milestone 4C — Fan-out and resource isolation (implemented)
Evidence: [FANOUT_ISOLATION.md](FANOUT_ISOLATION.md).

- 8 concurrent readers: 0 gaps, worst p99 1.5 ms;
- one reader frozen with SIGSTOP: starved to 254 while the healthy seven stayed
  at 522 minimum, no measurable coupling;
- **Fast DDS silently caps keyed instances at 10 by default** — 32-key topic
  delivered 10 of 32 with no error anywhere, and 32 of 32 once max_instances was
  raised. Raising it alone rejects the endpoint, so the pool is now sized as
  instances x history_depth;
- fixed a latent 4B bug that hardcoded max_instances = 1;
- one key at 20x did not starve unrelated keys, 0 staleness on the other seven;
- 5 reader-churn cycles: writer RSS +1%, still publishing. First observed
  process-resource evidence rather than a configured ceiling;
- controls mutated: not freezing the reader and not making the hot key hot both
  turn their assertions red.

Outstanding:
- fan-out combined with tc netem impairment;
- reader-side RSS/CPU;
- soak-duration churn rather than 5 cycles;
- multi-writer fan-in;
- instance scaling beyond 32.

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
