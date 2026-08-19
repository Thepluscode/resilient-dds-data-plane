# Milestone 4B — Writer-side bounded resources

Milestone 2's slow-consumer scenario watched the **reader** and could only report
that data arrived late and stale. It could not answer the producer's question:

> when the reader stops draining and the writer's history fills, what does
> `write()` do?

That answer is a QoS choice, and it decides whether the producer ever finds out
it is losing data.

```bash
docker run --rm -v "$PWD":/work rdtf-build bash -c \
  'cmake -S /work -B /tmp/b -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /tmp/b -j && BUILD=/tmp/b bash /work/scripts/run_backpressure_scenarios.sh'
```

## Result

400 Hz producer, reader deliberately capped near 250/s (4 ms per sample in the
listener callback), `periodic_telemetry` RELIABLE, 10 s.

One run, reproduced by the command above; the harness writes the same table to
`evidence/backpressure/matrix.md`, which CI uploads as a build artifact rather
than committing. Absolute figures move
between runs; the pattern across rows is what the experiment is about, and the
ranges observed over six runs are given underneath.

| case | written | timeout | out_of_res | blocked_max | recv | gaps |
|---|---|---|---|---|---|---|
| `KEEP_LAST(32)` | 4000 | **0** | 0 | 15.8 ms | 1654 | **23** |
| `KEEP_ALL`, max_samples 64, block 100 ms | 3984 | **16** | 0 | 105 ms | 1627 | 12 |
| `KEEP_ALL`, max_samples 64, **no blocking** | 2368 | **1632** | 0 | 9.5 ms | 1559 | 265 |
| `KEEP_ALL`, max_samples 512, block 100 ms | 3985 | 15 | 0 | 105 ms | 1398 | 6 |

Across runs: `KEEP_LAST` write failures were **always 0** and its reader gaps
ranged 21–23; `KEEP_ALL` with blocking failed 3–16 writes and always blocked to
~100 ms; the no-blocking variant rejected 1632–1847 writes and produced 185–361
gaps. The direction of every comparison held in all six runs.

### Timeout is not the only way a writer fails

`write()` returning failure is two distinct conditions, and one counter for both
hides the diagnosis:

- **`RETCODE_TIMEOUT`** — history was full and the writer waited out
  `max_blocking_time`. Costs the full budget. Fix is a slower producer, a faster
  reader, or accepting the loss.
- **`RETCODE_OUT_OF_RESOURCES`** — the writer could not allocate a cache change
  at all. Returns in microseconds, never waits. Fix is allocation headroom.

**Every failure in the table above is `TIMEOUT`, with zero `OUT_OF_RESOURCES`.**
That is a property of this experiment, not of DDS: setting `max_samples` also
reserves the allocation, so the writer always has somewhere to put the sample
and only ever waits. Starving the allocation instead produces the other mode —
see `scripts/run_resource_bounds.sh`, which drives `extra_samples=0` and gets
`OUT_OF_RESOURCES` failing in ~8 µs against `TIMEOUT` at ~50 ms.

The two harnesses are complementary: this one isolates history backpressure,
that one isolates allocation exhaustion. Reading either alone would support the
wrong conclusion about what to fix.

## What it says

**The reader received roughly 1400–1650 samples in every case.** That is its own processing
ceiling and no QoS setting moved it. The choice did not change throughput. It
changed **who finds out**.

- **`KEEP_LAST` loses data silently.** All 4000 writes returned success, and 23
  sequence gaps appeared at the reader. The writer overwrites its oldest
  unacknowledged sample to make room, so the producer is never told that
  anything was dropped. For a telemetry stream where the newest value
  supersedes the last, that is correct. For a command channel it means a lost
  command with no error anywhere in the system.

- **`KEEP_ALL` converts loss into backpressure.** The same overload produced 16
  timed-out writes and blocked the producer for up to 105 ms — the configured
  `max_blocking_time`. The producer can now see the condition and decide: shed
  load, alarm, buffer elsewhere, or fail over. Reader-visible gaps fell from 23
  to 12.

- **Failing fast is not the same as failing well.** With `max_blocking_time = 0`
  the producer learned immediately — 1632 of 4000 writes rejected — but the
  reader ended up with 265 gaps, twenty times worse than blocking. Note these
  are still `TIMEOUT`, not `OUT_OF_RESOURCES`: with a zero budget Fast DDS
  returns the timeout code without waiting.
  Refusing to wait does not create reader capacity; it just moves the loss
  earlier and makes it larger.

- **A bigger buffer did not help.** Raising `max_samples` from 64 to 512 left
  reader-visible gaps roughly halved (12 → 6) while `recv` fell from 1627 to 1398 —
  a worse outcome for the consumer, not a better one.
  Buffering absorbs a burst; it does nothing for a reader that is permanently
  slower than its writer, because there is no later moment at which the backlog
  drains.

The application-level rule this produces:

```text
write() returned true
        ≠
the sample will be delivered
```

which is the producer-side counterpart of the project's original thesis that a
delivered sample is not necessarily safe to use.

## Guard against unbounded memory

`KEEP_ALL` retains samples until acknowledged. With reliable delivery to a
reader that stops draining and no `max_samples`, writer memory grows without
limit and the failure arrives as an OOM kill rather than a QoS event. That is
refused up front:

```
QOS invalid: keep-all reliable history needs max_samples:
             an undrained reader grows writer memory without bound
```

The rule is deliberately narrow — best-effort never retains for
acknowledgement, so it does not require a ceiling, and there is a unit test
asserting the rule does **not** fire there.

## Controls, and what they caught

Four assertions, each observed failing before being trusted:

| control | mutation | result |
|---|---|---|
| `keep-all reliable with unlimited samples rejected` (unit) | disable the validate rule | FAIL, as required |
| `unbounded_keep_all_refused` (scenario) | disable the validate rule | FAIL, as required |
| `keep_last_silent_loss` | remove the overload (fast reader) | FAIL — needs real overload |
| `keep_all_backpressure` | remove the overload (fast reader) | FAIL — needs real backpressure |

The last one was wrong when first written. Its blocking floor was 1 ms, and an
**unloaded** run blocks ~1.2 ms in ordinary write latency, so it passed while
proving nothing. The floor is now 50 ms, far above ordinary latency and far
below the 100 ms `max_blocking_time` that real history-full blocking reaches.

## Fast DDS note

`ResourceLimitsQosPolicy` is rejected unless
`max_samples >= max_instances * max_samples_per_instance`. Bounding
`max_samples_per_instance` without also bounding `max_instances` fails
`check_allocation_consistency` and `create_datawriter` returns `nullptr`. This
lab pins `max_instances = 1` for its single keyed source.

## Not claimed

- writer memory was not measured directly; `max_samples` is asserted as a
  configured ceiling, not an observed RSS bound;
- one publisher, one subscriber, one keyed instance, small payload;
- single runs per cell, so the numbers show mechanism, not distribution;
- no coupling to `tc netem` impairment — this is pure consumer-side overload on
  a healthy link;
- `KEEP_LAST` blocked up to 15.8 ms and the no-blocking case up to 9.5 ms.
  Neither is history-full blocking — that path is absent in one and disabled in
  the other — so it is scheduling and ordinary write latency, reported rather
  than explained. Across runs it varied 2–16 ms, which is why the
  backpressure assertion's floor is 50 ms.
