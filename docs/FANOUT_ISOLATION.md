# Milestone 4C — Fan-out and resource isolation

Every earlier experiment used one publisher, one subscriber and one keyed
instance. That shape cannot answer the question this milestone exists for:

> can **one** unhealthy consumer, or **one** hot keyed instance, damage unrelated
> healthy consumers and unrelated state?

Isolation is the property under test. Throughput is not.

```bash
docker run --rm -v "$PWD":/work rdtf-fastdds:2.14.6 bash -c \
  'cmake -S . -B build-dds -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build build-dds -j && BUILD=build-dds ./scripts/run_fanout_scenarios.sh'
```

## Result

100 Hz publisher, `periodic_telemetry` RELIABLE, 8 readers, 10 s.

| scenario | result |
|---|---|
| 8 healthy readers | received 503–505 each, **0 gaps**, worst p99 1.50 ms |
| 1 of 8 frozen with `SIGSTOP` | frozen reader directly observed in process state `T`; healthy readers showed no measurable degradation |
| 32 keyed instances, default limits | **10 of 32 delivered** |
| 32 keyed instances, `max_instances=64` | **32 of 32 delivered** |
| 1 hot key (20×) + 7 normal | hot 1200, normal ≥60, **0 staleness on unrelated keys** |
| 5 reader churn cycles | writer RSS 14944 → 15144 kB (**+1%**), writer alive |

## The finding that matters: instance limits fail silently

**Fast DDS defaults `ResourceLimitsQosPolicy.max_instances` to 10.** Publish a
32-key topic and the first ten keys work while keys beyond that limit are never
delivered — no exception, no `on_sample_lost` and no anomaly from the
application trustworthiness layer. The keys simply do not exist at the reader.

The A/B is the proof:

```text
default max_instances   ->  10 / 32 keys
max_instances = 64      ->  32 / 32 keys
```

Without the first row the second proves nothing. The harness asserts the default
cap so a future middleware default change fails loudly rather than turning the
control into a meaningless green test.

Two consequences matter:

- **Raising it is correctness, not merely tuning.** A keyed topic sized for more
  than ten instances is incomplete until the resource limits are configured.
- **Raising it alone can break endpoint creation.** `max_samples` must be sized
  consistently with `max_instances` and per-instance history. The transport now
  derives the pool from the configured instance count and history depth rather
  than carrying the earlier single-instance assumption.

This also exposed a latent Milestone 4B assumption where the bounded-resource
path was originally written for one keyed instance. Fan-out testing forced that
assumption into the open.

## Isolation held everywhere it was tested

**A frozen reader did not measurably couple to the others.** One of eight readers
was stopped with `SIGSTOP` while the others continued receiving from the same
RELIABLE writer. The corrected control now proves the reader is actually stopped
by reading `/proc/<pid>/stat` and requiring process state `T` during the fault
window. The healthy readers did not show measurable degradation in the tested
single-host shape.

**A hot key did not starve unrelated state.** One key published at 20× the normal
rate produced 1200 samples against at least 60 for each of the seven normal keys,
with zero staleness anomalies on unrelated keys.

**Reader churn reclaimed cleanly in the short test.** Five start/kill cycles
against a running writer moved RSS from 14944 kB to 15144 kB, about +1%, with the
writer still publishing and no restart required. This is evidence for the tested
window only; it is not a soak-test claim.

## Controls — and the control that was wrong

"Nothing bad happened" is the easiest assertion to pass for the wrong reason.
This milestone therefore treats fault activation itself as something that must
be proven.

| control | mechanism | result |
|---|---|---|
| `fault_isolation` | read `/proc/<pid>/stat` and require process state `T` during the injected `SIGSTOP` | proves the reader is actually frozen instead of inferring the fault from delivery counts |
| `key_isolation` | mutation sets hot multiplier to 1 | assertion fails because the required overload ratio disappears |
| `instance_limit_is_silent` | default-limit run is the positive control for the configured 32-key run | proves the instance cap is the mechanism |
| `resource_reclamation` | unreadable RSS is a failure, not a pass | prevents missing measurement from looking like stable memory |

### Why sample count was an invalid freeze detector

The first fault-isolation control inferred "reader was frozen" from its final
sample count. That was wrong for a RELIABLE TRANSIENT_LOCAL writer. A reader can
be stopped, later resumed, drain retained backlog and finish with a count equal
to or even higher than readers that never stalled. The output proxy therefore
did not prove the injected condition.

The fix is direct observation: while the fault is active, the harness reads
`/proc/<pid>/stat` and requires Linux process state `T` (stopped/traced). The
control now proves the cause rather than guessing from an effect.

### Why the hot-key threshold changed

`key_isolation` was also wrong when first written. Its original assertion was
roughly `hot > cold_min`, which round-robin ordering can satisfy by one sample.
With the overload disabled it could still pass. It now requires a meaningful
ratio against the deliberately injected 20× multiplier.

This is the same testing lesson seen elsewhere in the repository: a plausible
threshold or proxy is not evidence until the mechanism has been removed and the
assertion is observed failing.

## Not claimed

- eight readers on one host, not eight hosts; no NIC or switch contention;
- no `tc netem` impairment combined with fan-out;
- RSS sampled from `/proc/<pid>/status`, writer only — reader-side memory and CPU
  are not measured;
- churn is five cycles over roughly ten seconds, not a soak test;
- one publisher throughout; multi-writer fan-in is not measured here;
- 32 instances is not a large-scale instance study.
