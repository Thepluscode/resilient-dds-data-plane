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
> processes over real RTPS, and it all runs in CI on every push.
>
> The question the whole project asks is: when things start to break, how do you
> know the state you are holding is still safe to act on?

---

## Act 1 — the silent instance cap (~35 s)

**Before it runs:**

> First one is the result I did not expect. I am publishing thirty-two distinct
> keyed instances. Nothing is misconfigured.

**When `10 / 32` appears — pause here, it is the whole act:**

> Ten. Twenty-two keys were never delivered. No error, no exception, no
> sample-lost callback, no anomaly from the detection layer I built. Every
> diagnostic in the project stays green while a third of the state does not
> exist.
>
> Fast DDS defaults `max_instances` to ten. It is a default, so nothing warns
> you — the topic is simply lossy until you set it.

**When `32 / 32` appears:**

> Same workload with `max_instances` raised. All thirty-two.
>
> The first run matters more than the second. Without it, "we deliver 32 keys"
> proves nothing — I would not know whether the limit was ever real. The test
> asserts the default caps at exactly ten, so if a future Fast DDS changes that,
> the suite fails loudly instead of quietly passing.

**If asked "how did you find it":** the fan-out suite had a scenario asking
whether resource limits are global or per-instance. It failed at 10 of 32 and I
went looking. Raising the limit alone then rejected the endpoint — Fast DDS
enforces `max_samples >= max_instances × max_samples_per_instance` — so the pool
is now sized as instances × history depth.

---

## Act 2 — loss becomes latency (~30 s)

**Before:**

> Fifteen percent packet loss, applied with `tc netem`. Same impairment, two QoS
> contracts. I expected loss to show up as missing samples.

**On the two lines:**

> It does not, under RELIABLE. Zero gaps — every sample arrived — but around
> sixty of them arrived stale, and the median latency is about 174 milliseconds
> against a 250 millisecond freshness budget. Retransmission recovered the data
> and spent almost the whole time budget doing it.
>
> Best-effort, same loss: thirty-two gaps, nothing stale, median latency about
> two hundred microseconds.
>
> Neither is safer. Reliable trades timeliness for completeness, best-effort
> trades completeness for timeliness. The right answer depends on whether this
> consumer would rather have old data or missing data — and only the application
> knows that. That is the argument for treating QoS as a contract rather than
> tuning.

---

## Act 3 — the unsafe window (~30 s)

**Before:**

> Full partition mid-stream, then restore. Watch the two timings.

**On the output:**

> The consumer's freshness budget broke about 260 milliseconds after the
> partition. DDS declared the writer lost at about a second. That gap — roughly
> three quarters of a second — is time in which the middleware was perfectly
> content and the state was already unusable.
>
> For a 20 millisecond control loop that is about thirty-seven cycles of acting
> on state that nothing had flagged. It is why the application freshness guard
> and the DDS liveliness callback are complementary rather than duplicated
> effort: a system that alarms only on liveliness has a silent unsafe window
> equal to its lease duration.

**If asked about recovery:** it comes back in tens of milliseconds, but that
number was wrong the first time I measured it. The writer flushes retained
history on restore and those samples are already stale; my health monitor
counted them as evidence of recovery and declared healthy while the detector was
flagging the very same samples. Recovery now only counts samples inside the
freshness budget.

---

## Act 4 — write() returned true (~30 s)

**Before:**

> Producer at 400 hertz, consumer that can only handle about 250 a second. No
> network fault at all. Same overload, two writer history contracts.

**On the output:**

> KEEP_LAST: zero write failures. Every single write returned success — and the
> reader still saw gaps. The writer overwrites its oldest unacknowledged sample
> to make room, so the producer is never told anything was dropped. For
> telemetry where the newest value supersedes the last, that is correct. For a
> command channel it is a lost command with no error anywhere in the system.
>
> KEEP_ALL: the same loss arrives as failed writes and blocking the producer can
> actually act on — shed load, alarm, fail over.
>
> So the producer-side version of the thesis: `write()` returning true does not
> mean the sample will be delivered.

---

## Close — 20 seconds

> The whole project is about that gap between what the middleware reports and
> what the application can safely believe. DDS tells you what happened to
> communication. It cannot tell you whether the state is still safe to use —
> that needs application invariants on top, and this repository is the evidence
> that those invariants catch things the middleware does not.
>
> It is honest about limits too. There is a list in the README of what is not
> proven: no RTI Connext, no certification, no statistical tail claims, no
> multi-writer fan-in.

---

## Questions to expect

**"Why DDS rather than Kafka?"** Kafka is a durable ordered log for asynchronous
replay. DDS is data-centric pub/sub for real-time distribution with endpoint QoS
contracts — deadline, liveliness, durability, ownership. This project uses those
semantics rather than treating DDS as a message bus.

**"Have you used RTI Connext?"** No, and the README says so. Vendor code sits
behind a pimpl seam so no other translation unit includes a Fast DDS header; a
Connext backend is a second `.cpp` implementing the same two classes.

**"What was hardest?"** Not the DDS API — making the tests mean anything. Two
assertions in this repo passed on noise until I mutation-tested them: a
blocking-time floor of 1 ms when ordinary write latency is 1.2 ms, and a hot-key
check satisfied by a single sample of round-robin ordering. Both looked correct
on the page. Now every guard is watched failing before it is trusted.

**"What would you do next?"** Fan-out combined with network impairment, reader-side
resource measurement, and a soak run. Then an RTI Connext adapter to prove the
seam is real.
