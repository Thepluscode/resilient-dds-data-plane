# Milestone 2 — Network Degradation & Recovery

Every number here came from `scripts/run_netem.sh`. Raw artefacts are in
`evidence/`.

```bash
docker build -t rdtf-build docker/
docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-build bash -c \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j && BUILD=/tmp/b bash /work/scripts/run_netem.sh'
```

## The transport must be forced

Fast DDS prefers shared memory between local endpoints. A `tc netem` qdisc on a
network interface does not touch shared memory, so the default configuration
would sail through every impairment below and produce a table of clean results
proving nothing.

Both endpoints therefore disable the builtin transports and install a single
`UDPv4TransportDescriptor` (`TransportMode::udp_only`, the default in
`dds_transport.cpp`). `--allow-shm` opts back out.

## Result matrix

50 Hz, 6 s per cell, UDP-only on `lo`. `recv` is samples the application saw;
`gaps` is application-visible sequence loss; `stale` is samples that arrived
outside the 250 ms freshness budget.

| impairment | profile | recv | gaps | stale | dl_miss | p50_us | p99_us | max_us |
|---|---|---|---|---|---|---|---|---|
| baseline | periodic_telemetry (RELIABLE) | 210 | 0 | 0 | 0 | 368 | 1029 | 1604 |
| loss 1% | periodic_telemetry | 211 | 0 | 2 | 1 | 392 | 246692 | 292468 |
| loss 15% | periodic_telemetry | 164 | 2 | 82 | 9 | 249249 | 751700 | 771628 |
| delay 50ms | periodic_telemetry | 207 | 0 | 0 | 0 | 51394 | 55518 | 58606 |
| jitter 50±50ms | periodic_telemetry | 212 | 0 | 0 | 0 | 77402 | 149014 | 167321 |
| reorder 20% | periodic_telemetry | 227 | 0 | 0 | 0 | 12032 | 20912 | 41069 |
| baseline | high_rate_sensor (BEST_EFFORT) | 211 | 0 | 0 | 0 | 330 | 1872 | 8950 |
| loss 1% | high_rate_sensor | 209 | 3 | 0 | 0 | 302 | 1001 | 12755 |
| loss 15% | high_rate_sensor | 171 | 31 | 0 | 9 | 324 | 1253 | 1288 |
| delay 50ms | high_rate_sensor | 210 | 0 | 0 | 0 | 51086 | 54831 | 55094 |
| jitter 50±50ms | high_rate_sensor | 113 | 61 | 0 | 45 | 19394 | 105845 | 106693 |
| reorder 20% | high_rate_sensor | 225 | 0 | 0 | 0 | 12110 | 13073 | 23037 |

## What the matrix says

**Packet loss does not produce sequence gaps under RELIABLE — it produces
latency.** At 15% loss the reliable reader logged **0–2 application gaps and 82
stale samples**, with p50 latency at 249 ms against a 250 ms freshness budget.
Retransmission recovered the data and spent the time budget doing it. The same
impairment on the best-effort reader produced **31 gaps and 0 stale samples**,
with p99 latency unchanged at 1.2 ms.

That is the entire QoS argument in one row pair. Neither is "better":

- reliable trades **timeliness** for **completeness** — correct for state you must
  not lose, fatal for a control loop with a hard deadline;
- best-effort trades **completeness** for **timeliness** — correct when the newest
  reading supersedes the last one, fatal for a command you cannot drop.

An assertion of the form `packet loss => sequence gaps` would have been
conceptually wrong for half this table, which is why the harness asserts only
that data moved and reports the rest as measurements.

**Reordering never reached the application.** 20% reorder produced zero
`out_of_order` anomalies on either profile: RTPS reassembles by sequence number
in the reader cache. The application-level ordering guard is a backstop against
producer and clock faults, not transport reordering.

**Jitter hurts best-effort far more than loss does.** 50±50 ms of jitter cost the
best-effort reader 61 gaps and 45 deadline misses — worse than 15% flat loss.
Variance, not average delay, is what breaks a deadline contract.

## Partition and recovery

9-second total partition (`netem loss 100%`), reliable profile, 50 Hz.

```text
    t=  1017 ms  healthy   none
    t=  7277 ms  degraded  stale_data      <-- partition applied at t=7015
    t=  8020 ms  lost      liveliness_lost
    t= 16091 ms  healthy   none            <-- partition removed at t=16046

    time_to_unsafe (degraded)  = 262 ms
    time_to_lost               = 1005 ms
    recovery_to_healthy        = 45 ms
```

Timings are anchored on an `EPOCH_MS` line the subscriber emits at its own t=0.
An earlier version compared the harness's wall clock against the subscriber's
frame and understated recovery by the DDS initialisation time.

### The number worth arguing about: time-to-unsafe

**The consumer's state became unusable 743 ms before DDS declared the writer
lost.** Freshness breached at +262 ms; liveliness expired at +1005 ms.

For a 20 ms control loop that gap is roughly 37 cycles of acting on state the
middleware still considered fine. The DDS health callbacks and the application
freshness guard are therefore complementary rather than redundant, and a system
that alarms only on liveliness has a silent unsafe window equal to its lease
duration.

### RELIABLE is not lossless

During the partition the middleware reported **353 samples lost** and the
application saw a single gap of 353. The writer's `KEEP_LAST(32)` history
overwrote everything it could not deliver. Reliability is bounded by history
depth and resource limits; it is a delivery contract, not a guarantee of
completeness across an outage.

### The recovery number nearly lied

The first implementation reported `recovery_to_healthy = 27 ms` after a
nine-second partition. That was wrong, and the harness caught it: on restore the
writer flushes its retained history, and those retransmitted samples are already
stale. `HealthMonitor::on_sample` counted them toward the three-consecutive-fresh
recovery rule, so the monitor declared HEALTHY while the detector was
simultaneously flagging the very same samples stale.

`on_sample` now takes the sample's age and refuses to count anything outside the
freshness budget as recovery evidence. In the log above, samples 608–628 arrive,
are all flagged stale, and health stays `lost` until a sample with `age_ms=0`
appears. The fix is mutation-tested: disabling the age guard turns the
regression assertion red.

## Slow consumer

Publisher at 500 Hz, subscriber sleeping 4 ms per sample in the listener
callback. **No network impairment at all.**

| profile | recv | gaps | stale | middleware lost | p99 latency |
|---|---|---|---|---|---|
| periodic_telemetry (RELIABLE) | 1580 | 22 | 1466 | 1382 | 1.14 s |
| high_rate_sensor (BEST_EFFORT) | 1557 | 21 | 1450 | 1365 | 1.16 s |

A perfectly healthy network, nothing offline, no liveliness event — and p99
latency over a second with 93% of received samples stale. The two QoS profiles
are indistinguishable here, because the bottleneck is the reader's own thread:
**no delivery contract can fix a consumer that is slower than its producer.**

This is the failure mode a network-focused test plan misses entirely, and the
one where a health model built only on DDS callbacks reports nothing wrong.

## Limits of these numbers

- single host, containerised on Docker Desktop for macOS (a Linux VM). Absolute
  latencies are not bare-metal figures; the comparisons between rows are the
  point, not the values.
- impairment applied to `lo`, which would affect discovery traffic as well as
  data, so each cell now waits for the endpoints to match on a clean link before
  the qdisc goes on. Without that, 15% loss intermittently dropped participant
  announcements and the cell reported "moved no data at all" — a discovery
  failure wearing the costume of a delivery result. It was seen twice in CI and
  turned `main` red on a docs-only merge before being diagnosed. The consequence
  is that roughly the first second of each cell is unimpaired.
- one publisher, one subscriber, one keyed instance, small payload.
- 6 s per cell and a single run per cell — enough to show mechanism, not enough
  for statistical claims about the tail. Repeated runs are still outstanding.
- latency derives from the publisher's `source_timestamp`; valid only because
  both processes share a clock.
