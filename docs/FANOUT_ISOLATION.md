# Milestone 4C — Fan-out and resource isolation

Every earlier experiment used one publisher, one subscriber, one keyed instance.
That shape cannot answer the question this milestone exists for:

> can **one** unhealthy consumer, or **one** hot keyed instance, damage
> unrelated healthy consumers and unrelated state?

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
| 1 of 8 frozen with `SIGSTOP` | frozen starved to 254; healthy readers **522 min, no degradation** |
| 32 keyed instances, default limits | **10 of 32 delivered** |
| 32 keyed instances, `max_instances=64` | **32 of 32 delivered** |
| 1 hot key (20×) + 7 normal | hot 1200, normal ≥60, **0 staleness on unrelated keys** |
| 5 reader churn cycles | writer RSS 14944 → 15144 kB (**+1%**), writer alive |

## The finding that matters: instance limits fail silently

**Fast DDS defaults `ResourceLimitsQosPolicy.max_instances` to 10.** Publish a
33-key topic and the first ten keys work perfectly while keys 10–31 are never
delivered at all — no error, no exception, no `on_sample_lost`, no anomaly from
the trustworthiness layer. The keys simply do not exist, and every diagnostic in
this project stays green.

The A/B is the whole proof, and the default run is the control:

```text
default max_instances   ->  10 / 32 keys
max_instances = 64      ->  32 / 32 keys
```

Without the first row the second proves nothing. The harness asserts the default
caps at exactly 10, so if a future Fast DDS changes that default the suite fails
loudly instead of quietly passing.

Two consequences worth carrying:

- **Raising it is not tuning, it is correctness.** A keyed topic sized for more
  than ten instances is silently lossy until `max_instances` is set.
- **Raising it alone breaks the endpoint.** Fast DDS enforces
  `max_samples >= max_instances * max_samples_per_instance`, and the defaults
  (5000 and 400) cannot satisfy 64 instances. Setting `max_instances` without
  also sizing the pool makes `create_datawriter` return `nullptr`. The transport
  now derives the per-instance share from `history_depth` and sizes
  `max_samples` as `instances × depth`.

This also fixed a latent bug introduced in Milestone 4B, where the resource
limits path hardcoded `max_instances = 1` for the then single-key lab. That
would have silently reduced any bounded-resource multi-key topic to one
instance.

## Isolation held everywhere it was tested

**A frozen reader did not couple to the others.** `SIGSTOP` on one of eight
readers — it stops draining entirely while remaining matched and alive to DDS,
which is the case a RELIABLE writer must absorb. The frozen reader starved to
254 samples; the seven healthy readers stayed at 522 minimum, slightly *above*
the 8-healthy baseline of 503. No measurable degradation.

**A hot key did not starve unrelated state.** One key published at 20× produced
1200 samples against ≥60 for each of the other seven, and **zero staleness
anomalies on any unrelated key**.

Both results are negative findings, which is exactly why each needed a control
proving the fault was real — see below.

**Reader churn reclaimed cleanly.** Five start/kill cycles against a running
writer moved RSS from 14944 kB to 15144 kB, +1%, with the writer still
publishing and no restart required. This is the first milestone to turn a
configured resource ceiling into observed process-resource evidence; 4B measured
only what it had configured.

## Controls

"Nothing bad happened" is the easiest assertion to pass for the wrong reason, so
each isolation claim was mutated:

| control | mutation | result |
|---|---|---|
| `fault_isolation` | don't actually `SIGSTOP` the reader | FAIL — "frozen reader got 466 vs healthy 465, the freeze did nothing" |
| `key_isolation` | set hot multiplier to 1 | FAIL — "hot key got 61 vs normal 60, under 5x" |
| `instance_limit_is_silent` | is itself the control for `many_keys` | — |
| `resource_reclamation` | RSS unreadable is a failure, not a pass | — |

`key_isolation` was **wrong when first written**. Its test was `hot > cold_min`,
which round-robin ordering satisfies by a single sample: with the overload
switched off entirely it passed at `hot=62 vs normal=61`. It now requires a 5×
ratio against a 20× injected multiplier. This is the second threshold in this
project to pass on noise — the first was a 1 ms blocking floor in 4B against
~1.2 ms of ordinary write latency. Both were caught only by running the
mutation, never by reading the code.

## Not claimed

- eight readers on one host, not eight hosts; no NIC or switch contention;
- no `tc netem` impairment combined with fan-out;
- RSS sampled from `/proc/<pid>/status`, writer only — reader-side memory and
  CPU are not measured;
- churn is five cycles over ~10 s, which is not a soak test and says nothing
  about slow leaks;
- one publisher throughout; multi-writer fan-in is untested;
- 32 instances is not "many" — instance scaling beyond that is unmeasured.
