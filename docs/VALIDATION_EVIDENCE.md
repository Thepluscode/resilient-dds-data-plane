# Validation Evidence

Last regenerated: 2026-08-18, by running the commands shown. Every number below
was produced by the tooling on this page; none is estimated.

## Environment

| | |
|---|---|
| Container | `ubuntu:24.04` (`docker/Dockerfile`) |
| Compiler | g++ 13.3.0, C++17 |
| CMake | 3.28.3 |
| DDS | Fast DDS (`libfastrtps-dev`) **2.11.2+ds-6.1build3** |
| IDL codegen | Fast DDS-Gen **2.3.0** |
| Host | Apple Silicon macOS running Docker Desktop |

The macOS host toolchain is **not** used. Its Command Line Tools install is
missing libc++ headers (`/Library/Developer/CommandLineTools/usr/include/c++/v1`
contains 3 files), so `#include <cstdint>` fails for any program. Everything is
built and run in the container. That is also the correct environment for DDS:
process kills and network namespaces are the failure modes under test.

## Compiler gate

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

Core library built with `-Wall -Wextra -Wpedantic -Werror`. Result: **PASS**.

## Unit test gate

```bash
ctest --test-dir build --output-on-failure
```

Result: **PASS** — `100% tests passed, 0 tests failed out of 1`.

The core suite covers the detector and health state machine only. It does not
touch DDS; the DDS behaviour is covered by the scenario harness below.

## Sanitizer gate

```bash
cmake -S . -B san -DCMAKE_BUILD_TYPE=Debug -DRDTF_ENABLE_SANITIZERS=ON
cmake --build san -j && ctest --test-dir san --output-on-failure
```

AddressSanitizer + UndefinedBehaviorSanitizer. Result: **PASS**, no findings.

Scope limit: this covers the unit-test path. The DDS transport has **not** been
run under sanitizers — Fast DDS's own threads make that a separate exercise.

## Live DDS failure-injection scenarios

```bash
docker build -t rdtf-build docker/
docker run --rm -v "$PWD":/work rdtf-build bash -c \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j && BUILD=/tmp/b bash /work/scripts/run_scenarios.sh'
```

Each scenario is two real processes exchanging real RTPS traffic on an isolated
domain. Result: **10 passed, 0 failed**.

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

Two controls stop this passing vacuously:

- **baseline** asserts a *minimum* sample count (>=100), so "0 anomalies" cannot
  come from a run that moved no data.
- **negative_control** asserts `DEADLINE_MISSED`, `LIVELINESS_LOST` and
  `INCOMPATIBLE_QOS` are all **absent** on the healthy stream, so matching them
  in the failure scenarios means something.

Both controls have already caught real defects. The negative control failed on
its first run: the baseline publisher stopped 5 s before the subscriber's window
closed, and that trailing silence was a genuine deadline miss. The baseline
count assertion then caught a second one — the publisher and subscriber windows
ended at the same moment, so the received count raced the clock and varied
between 124 and 140 across runs. The publisher now outlasts the subscriber and
is killed by the harness. Three consecutive full runs: 10/10 each.

## Two findings worth stating

**A clean process exit is not a liveliness loss.** Terminating the publisher
normally unmatches the writer and the reader reports `subscription_matched=0` —
no liveliness callback fires. Only `SIGKILL`, which sends no goodbye message,
leaves the lease to expire. The scenario now kills the publisher hard, because
the crashed-process case is the one that matters operationally.

**Transient-local replay delivers data that is not fresh.** A reader joining 3 s
late received all history from sequence 1 — and 25 of those replayed samples
immediately breached the freshness budget. Delivery succeeded; the state was not
safe to act on. This is the project's whole thesis, observed rather than argued.

## Two-container transport check

```bash
docker compose -f docker/docker-compose.yml up --abort-on-container-exit
```

Inside a single container Fast DDS selects its shared-memory transport and never
touches UDP, so a one-process demo proves nothing about RTPS on the wire. This
runs publisher and subscriber in separate containers on a bridge network.
Observed: discovery completed, `rdtf_samples_published_total 300`, and the
injected gap, duplicate and stale faults were each detected on the far side.

## Measured performance

### Application validation path (no DDS)

```text
samples=1000000
anomalies=0
elapsed_seconds=0.0197341
samples_per_second=50673683
```

This is **not** a DDS throughput figure. It measures only the
sequence/freshness/schema checks, and exists to show that layer is not the
bottleneck.

### End-to-end DDS latency (real)

Publisher and subscriber as separate processes, 1200 samples at 200 Hz,
`periodic_telemetry` profile (RELIABLE / TRANSIENT_LOCAL / KEEP_LAST 32):

```text
samples_received      = 1200 / 1200
latency_min_us        = 36
latency_mean_us       = 189
latency_p99_us        = 625
latency_max_us        = 3898
```

Caveats that must be stated with these numbers:

- computed from the publisher's `source_timestamp`, so it is only valid because
  both processes share a clock. Across hosts this needs NTP or PTP discipline.
- measured inside Docker Desktop on macOS — a Linux VM. Bare metal will be
  faster and, more importantly, will have a different tail.
- p99 uses nearest-rank over the whole run; there is no warm-up exclusion.

## Milestone 2 — network degradation

Full matrix, analysis and caveats: [NETWORK_DEGRADATION.md](NETWORK_DEGRADATION.md).
14 impairment runs, 0 failed. Headline results:

- 15% loss, RELIABLE: 0 application gaps, 82 stale, p50 249 ms.
- 15% loss, BEST_EFFORT: 31 gaps, 0 stale, p99 1.25 ms.
- partition: unsafe at +262 ms, DDS reported lost at +1005 ms — a 743 ms window
  in which the middleware was still content and the state was not usable.
- slow consumer on a healthy network: p99 1.14 s, 93% of samples stale.

Two defects were found by the harness rather than by reading the code:

1. **The recovery counter accepted stale samples.** Post-partition retransmission
   flushes retained history whose samples are already outside the freshness
   budget. `HealthMonitor::on_sample` counted them toward the
   three-consecutive-fresh rule and declared HEALTHY 27 ms after restore, while
   the detector was flagging the same samples stale. Fixed by passing sample age
   into the monitor. Mutation-tested: disabling the age guard turns
   `stale retransmission burst does not restore health` red, restoring it turns
   it green.
2. **Two clock origins in the recovery measurement.** The harness timed faults on
   its own wall clock while `HEALTH_AT` was relative to the subscriber's post-init
   start, understating recovery by the DDS initialisation time. The subscriber
   now emits `EPOCH_MS` and the analyser reconciles the frames.

Unit assertions: 25, all passing. The two new blocks cover hysteresis in both
directions and the stale-recovery regression.

## Still not proven

Do not claim any of these:

- statistical claims about the latency tail — one run per impairment cell;
- latency under CPU contention or with many participants;
- bounded resource limits, writer-history exhaustion, large payloads;
- multiple publishers or multiple keyed instances;
- DDS Security: authentication, access control, encryption — not implemented;
- malformed/adversarial payload handling;
- Fast DDS 3.x compatibility — the code is built against the 2.11 API;
- RTI Connext — no adapter written, no licence, nothing attempted;
- sanitizer cleanliness of the DDS transport path.
