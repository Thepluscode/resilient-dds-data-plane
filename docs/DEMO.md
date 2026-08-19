# Demo script — ~3 minutes spoken over a ~60 second run

```bash
docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work rdtf-fastdds:2.14.6 bash -c \
  'cmake -S . -B build-dds -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null &&
   cmake --build build-dds -j >/dev/null && PAUSE=1 BUILD=build-dds ./scripts/demo.sh'
```

`PAUSE=1` waits for a keypress between acts so you can talk over each result.
Without it the whole thing runs unattended in about a minute — useful for a
screen recording.

**Numbers move slightly between runs.** Say "about" and read what is actually on
screen. The direction of every comparison is stable; the exact figures are not.

---

## Opening — 15 seconds

> This is a C++17 reliability lab built on Fast DDS. It is not a publisher and
> subscriber demo — the interesting part is what happens when the assumptions
> underneath one start to fail. Everything you are about to see runs as real
> processes over real RTPS, and the evidence chain is reproduced in CI.
>
> The question the whole project asks is: when things start to break, how do you
> know the state you are holding is still safe to act on?

---

## Act 1 — the silent instance cap

**Before it runs:**

> First one is the result I did not expect. I am publishing thirty-two distinct
> keyed instances.

**When `10 / 32` appears:**

> Ten. Twenty-two keys were never delivered. No exception, no sample-lost
> callback and no anomaly from my detection layer. Every diagnostic can stay
> green while most of the expected state simply does not exist.
>
> Fast DDS defaults `max_instances` to ten. The control run proves the default is
> the mechanism; the configured run raises the instance and sample pool limits
> and delivers all thirty-two.

**If asked how it was found:** the fan-out experiment was specifically testing
resource isolation across keyed state. Raising `max_instances` alone then exposed
a second constraint: `max_samples` has to be sized consistently with instances
and per-instance history.

---

## Act 2 — loss becomes latency

**Before:**

> Fifteen percent packet loss, injected with `tc netem`. Same impairment, two QoS
> contracts. I expected loss to show up as missing samples.

**On the two lines:**

> Under RELIABLE it can show up as latency instead. In a representative run,
> gaps stayed at zero while dozens of samples became stale and median latency
> moved into the hundreds of milliseconds.
>
> Under BEST_EFFORT the same loss appeared as visible gaps while latency stayed
> around hundreds of microseconds.
>
> Neither policy is automatically safer. The application has to decide whether
> old data or missing data is the less dangerous failure mode. That is why this
> project treats QoS as an application contract rather than middleware tuning.

---

## Act 3 — the unsafe window

**Before:**

> Full partition mid-stream, then restore. Watch the freshness and liveliness
> timings separately.

**On the output:**

> The application freshness budget breaks a few hundred milliseconds after the
> partition, while DDS liveliness loss arrives around a second. Across observed
> runs that leaves roughly three quarters of a second where communication health
> has not yet declared the writer lost but the state is already unsafe to use.
>
> For a 20 millisecond control loop that is dozens of cycles. Freshness and
> liveliness therefore answer different questions; one cannot replace the other.

**If asked about recovery:** retained history made the first recovery measurement
wrong. Replayed samples arrived after connectivity returned but were already
stale. Recovery now requires a fresh sample, not merely renewed delivery.

---

## Act 4 — `write()` returned true

**Before:**

> Producer faster than its consumer, with no network fault. Same overload, two
> history contracts.

**On the output:**

> `KEEP_LAST` can return success while the reader still observes gaps because old
> unacknowledged state is overwritten. That can be correct for telemetry where
> the newest value supersedes the previous one, but it would be a dangerous
> contract for non-idempotent command semantics.
>
> Bounded `KEEP_ALL` makes the overload producer-visible through blocking,
> timeout or resource exhaustion. The project separately proved immediate
> `OUT_OF_RESOURCES` and bounded `TIMEOUT` paths instead of collapsing both into
> one generic publish error.
>
> So the producer-side version of the thesis is: `write()` returning true does
> not prove the sample will arrive.

---

## Close — 20 seconds

> The project is about the gap between what middleware reports and what an
> application can safely believe. DDS gives strong communication contracts, but
> application invariants still have to decide whether state is fresh, complete,
> authorized and usable.
>
> The engineering roadmap is deliberately frozen now. The repository already
> demonstrates live RTPS, QoS failure semantics, security, authority/failover,
> bounded resources and fan-out isolation. The next work is evidence conversion:
> this demo, interview material and role-specific proof — not adding features for
> their own sake.

---

## Questions to expect

**"Why DDS rather than Kafka?"** Kafka is a durable ordered log for asynchronous
replay. DDS is data-centric pub/sub for real-time distribution with endpoint QoS
contracts such as deadline, liveliness, durability, ownership and resource
limits. This project uses those semantics rather than treating DDS as a generic
message bus.

**"Have you used RTI Connext?"** No. The implementation is Fast DDS 2.14.6 and the
README says so. Vendor headers are isolated behind a pimpl seam so a Connext
backend can be added if a programme requires it without rewriting the
application-level trustworthiness layer.

**"What was hardest?"** Making the tests prove the thing they claimed to prove.
Several controls were wrong in ways code review did not reveal: a blocking floor
that passed on ordinary scheduling latency, a hot-key ratio that passed on one
round-robin sample, and a fan-out fault control that inferred `SIGSTOP` from
sample counts even though a resumed TRANSIENT_LOCAL reader could drain retained
history and finish ahead of readers that never stalled. The corrected control
checks `/proc/<pid>/stat` for process state `T` directly.

The merge gate had the same class of defect. It once looked at PR checks spanning
multiple commits, so stale results could influence a new head. It now resolves
the exact head SHA, considers only runs for that SHA, and fails closed when no
matching runs exist. A confident check against the wrong state is still a false
control.

**"What would you do next?"** For this portfolio repository, stop expanding it.
I would add RTI Connext only if a role or programme makes interoperability a real
requirement. Otherwise the highest-value next step is applying the proven work:
record the demo, use the evidence in interviews, and let real engineering needs
determine the next experiment.
