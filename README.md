# Resilient DDS Data Plane

**C++17 · DDS/RTPS · Fast DDS 2.14.6 · IDL · QoS · distributed systems · security · failure injection**

A reliability lab built around one question:

> **When distributed communication still appears healthy, is the state actually
> safe to act on?**

This is not a publisher/subscriber tutorial. The project uses real Fast DDS
processes and RTPS traffic, then deliberately breaks assumptions around timing,
networking, identity, authority, resource limits and fan-out. Application-level
freshness, sequence, schema and health invariants decide whether received state
is still usable.

The engineering roadmap is now **frozen**. The repository has enough surface area
to demonstrate the systems problem; the next objective is to convert the proven
work into a demo, interview evidence and role-specific discussion rather than add
features for their own sake.

---

## Three results worth attention

### 1. A keyed topic can silently lose most of its state

Fast DDS defaults `ResourceLimitsQosPolicy.max_instances` to **10**. In the
32-key fan-out experiment:

```text
default max_instances   ->  10 / 32 keys delivered
max_instances = 64      ->  32 / 32 keys delivered
```

The missing instances did not present as a normal application gap, exception or
sample-loss anomaly. Raising the instance limit also required the sample pool to
be sized consistently with instance count and history depth.

### 2. RELIABLE packet loss can become latency rather than gaps

Under the same 15% injected packet loss, representative runs show different
failure shapes:

```text
RELIABLE      gaps=0        stale≈dozens    p50≈hundreds of ms
BEST_EFFORT   gaps≈dozens   stale=0         p50≈hundreds of µs
```

RELIABLE can recover missing transport data and spend the application's freshness
budget doing it. BEST_EFFORT can stay timely while exposing missing samples.
Neither is automatically safer; the application has to decide whether old data
or missing data is the less dangerous contract violation.

### 3. The application can become unsafe before DDS declares the writer lost

Across partition runs, freshness was breached a few hundred milliseconds after
the fault while DDS liveliness loss arrived around a second. Observed runs left
roughly three quarters of a second where middleware loss detection had not fired
but the state was already too stale to use.

For a 20 ms control loop, that is dozens of cycles.

The thesis:

```text
MESSAGE DELIVERED        != STATE SAFE TO USE
write() RETURNED SUCCESS != THE SAMPLE WILL ARRIVE
GREEN CHECK              != THE RIGHT STATE WAS CHECKED
```

---

## See it in about a minute

```bash
docker build -t rdtf-fastdds:2.14.6 -f docker/Dockerfile .

docker run --rm --cap-add=NET_ADMIN -v "$PWD":/work \
  rdtf-fastdds:2.14.6 bash -c \
  'cmake -S . -B build-dds -DRDTF_ENABLE_FASTDDS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null &&
   cmake --build build-dds -j >/dev/null && BUILD=build-dds ./scripts/demo.sh'
```

`PAUSE=1` stops between acts for a live walkthrough. Narration and likely
interview questions are in [docs/DEMO.md](docs/DEMO.md).

---

## Evidence map

| Area | What is demonstrated | Evidence |
|---|---|---|
| Live DDS/RTPS | IDL-generated types, participants, topics, writers/readers, discovery, real process traffic | [VALIDATION_EVIDENCE.md](docs/VALIDATION_EVIDENCE.md) |
| Failure semantics | gaps, duplicates, stale state, schema drift, deadline miss, hard writer death, late join, QoS mismatch | [VALIDATION_EVIDENCE.md](docs/VALIDATION_EVIDENCE.md) |
| Network degradation | loss, delay, jitter, reordering, partition/recovery, time-to-unsafe | [NETWORK_DEGRADATION.md](docs/NETWORK_DEGRADATION.md) |
| DDS Security | PKI-DH authentication, signed governance/permissions, unauthorized and untrusted denial, encrypted application traffic | [DDS_SECURITY.md](docs/DDS_SECURITY.md) |
| Authority/failover | SHARED control, EXCLUSIVE ownership, primary/standby writer strength, hard-failure takeover | [ROADMAP.md](docs/ROADMAP.md) |
| Writer resource bounds | allocation exhaustion vs bounded history timeout, recovery, KEEP_LAST vs bounded KEEP_ALL | [BOUNDED_RESOURCES.md](docs/BOUNDED_RESOURCES.md) |
| Fan-out/resource isolation | 8 readers, frozen-reader isolation, hot-key isolation, keyed instance limits, short churn/RSS observation | [FANOUT_ISOLATION.md](docs/FANOUT_ISOLATION.md) |
| Interview narrative | concise explanations, trade-offs and non-claims | [INTERVIEW_BRIEF.md](docs/INTERVIEW_BRIEF.md) |

---

## Architecture

```text
Producer / sensor / subsystem
          |
          v
   IDL-defined data model
          |
          v
+---------------------------+
| DDS DataWriter            |
| reliability / durability  |
| deadline / liveliness     |
| ownership / resources     |
| auth / permissions        |
+---------------------------+
          |
       DDS/RTPS
          |
          v
+---------------------------+
| DDS DataReader            |
| middleware callbacks      |
+---------------------------+
          |
          v
+---------------------------+
| Trustworthiness pipeline  |
| sequence guard            |
| freshness guard           |
| schema guard              |
| health state + reason     |
+---------------------------+
       |           |
       v           v
   metrics       JSONL evidence
```

Fast DDS-specific types are isolated behind the transport implementation rather
than spread through the application-level detector/health code. The public
implementation is **Fast DDS 2.14.6**; the repository does not claim RTI Connext
experience or interoperability.

---

## QoS is treated as an application contract

Three named profiles make different trade-offs explicit:

- **critical_control** — RELIABLE, TRANSIENT_LOCAL, strict deadline and explicit
  liveliness expectations;
- **periodic_telemetry** — RELIABLE, TRANSIENT_LOCAL, deeper retained history;
- **high_rate_sensor** — BEST_EFFORT, VOLATILE, shallow history where newest data
  matters more than retransmitting old values.

The experiments show why the choice matters:

- RELIABLE can turn loss into delay and staleness;
- BEST_EFFORT can turn loss into visible gaps;
- KEEP_LAST can return successful writes while old unacknowledged state is
  overwritten;
- bounded KEEP_ALL can expose overload to the producer through blocking, timeout
  or resource exhaustion;
- durability can successfully replay state that is already too stale to use.

---

## Security evidence

The security harness generates disposable test credentials and exercises six
cases:

```text
PASS secure_baseline
PASS untrusted_identity
PASS unauthorized_writer
PASS insecure_peer
PASS plaintext_capture_control
PASS encrypted_payload_capture
```

The paired packet-capture control matters. The plaintext path proves the capture
can see the unique application marker; the encrypted path then proves that marker
is absent while secure samples are still delivered. This is intentionally a
narrow claim: security-handshake metadata such as certificate identity can still
be observable.

---

## Authority and failure recovery

The authority experiment begins with SHARED ownership as a positive control so
the instrumentation has to prove it can see both writers. It then switches to
EXCLUSIVE ownership with a stronger primary and weaker standby.

Across verified runs, hard primary failure produced standby visibility in **under
110 ms** (81–103 ms observed across CI and a contended laptop), and the first
visible standby sample remained inside the freshness budget.

Correct authority with stale takeover state would still be unsafe, so takeover
latency and first-sample freshness are measured separately.

---

## Writer overload: two different failure paths

The bounded-resource work deliberately separates failures that are often lumped
together as "publish failed":

1. **Allocation pool exhaustion** — `RETCODE_OUT_OF_RESOURCES` can occur quickly
   when no cache-change reservoir remains.
2. **Reliable-history backpressure** — with allocation headroom available, a
   writer can wait for acknowledgements and then return `RETCODE_TIMEOUT` near
   its configured `max_blocking_time`.

The experiment also compares history contracts under a deliberately slower
reader. KEEP_LAST can keep returning success while the reader observes gaps;
bounded KEEP_ALL makes the overload visible to the producer.

The first version of the experiment was wrong: `extra_samples=0` exhausted the
allocation pool even in the intended healthy control. That failure was retained
in the PR history because it exposed the distinction the corrected A/B experiment
was supposed to measure.

---

## Fan-out and resource isolation

The fan-out milestone asks whether one unhealthy consumer or one hot keyed
instance damages unrelated state.

In the tested single-host shape:

- 8 healthy readers received with zero gaps;
- freezing one reader did not measurably degrade the others;
- one 20× hot key did not make unrelated keys stale;
- 32 keyed instances exposed the default `max_instances=10` cap;
- five short reader churn cycles left the writer alive with roughly +1% observed
  RSS movement in that run.

These are mechanism tests, not scale guarantees. See
[docs/FANOUT_ISOLATION.md](docs/FANOUT_ISOLATION.md) for the explicit boundaries.

---

## How the evidence is kept honest

Green tests are cheap. The harder part is proving that a green test answers the
question it claims to answer. Several controls in this repository were wrong in
ways that reading the code did not reveal.

- **Positive controls prove the system actually did work.** "Zero anomalies" is
  meaningless if zero data moved.
- **Negative controls prove healthy paths stay quiet.** Fault markers such as
  deadline/liveliness/QoS events must be absent when the fault is not present.
- **Absence claims need a positive visibility control.** The encrypted-pcap test
  is paired with a plaintext run that proves the capture can see the marker.
- **Assertions are mutation-tested.** A 1 ms blocking floor passed on ordinary
  scheduling latency, and a hot-key threshold passed on one sample of
  round-robin ordering. Both were tightened only after the mechanism was removed
  and the control was observed failing.
- **Measure the condition, not a proxy.** The fan-out fault control originally
  inferred "reader frozen" from final sample counts. With RELIABLE
  TRANSIENT_LOCAL delivery, a resumed reader drained retained backlog and was
  observed finishing ahead of readers that never stalled (649 vs 453). The
  corrected control reads `/proc/<pid>/stat` and requires Linux process state
  `T` while `SIGSTOP` is active.
- **CI evidence is bound to the exact head SHA.** The merge gate originally
  evaluated PR checks that could include stale results from earlier heads. A
  stale failure could block valid code; in the dangerous direction, a stale
  success could admit a broken head. The checked-in
  [`scripts/merge_gate.sh`](scripts/merge_gate.sh) now resolves the current head
  SHA, considers only workflow runs for that SHA and fails closed when no
  matching runs exist.

The general rule:

> **A gate is only valid if it proves the exact state it claims to gate. A
> correct check against the wrong state is still a false control.**

```text
confident answer != answer to the question actually asked
```

That is the same class of failure the DDS experiments expose: middleware can
answer a real communication-health question correctly while the application
needed a different safety question answered.

---

## What is deliberately not claimed

- RTI Connext implementation, SDK validation or interoperability;
- Fast DDS 3.x compatibility;
- security or safety certification;
- production certificate rotation, revocation, HSM-backed keys or zero-downtime
  credential rollover;
- arbitrary malformed/hostile RTPS resistance;
- statistical tail-latency guarantees across hardware populations;
- multi-host fan-out, NIC or switch contention results;
- large-payload or deterministic allocator guarantees;
- production split-brain/election behavior;
- production admission-control policy;
- sanitizer coverage of every live DDS transport thread.

The gap list is part of the evidence. A portfolio project that cannot state what
it has **not** proven is not strong evidence.

---

## Portfolio status

**Engineering roadmap: frozen.**

The next work is intentionally outside the feature backlog:

1. record the ~3 minute technical walkthrough;
2. use the verified findings as CV and interview evidence;
3. pin and present the repository as a systems-engineering project;
4. only reopen engineering when a real role, reviewer or programme exposes a
   missing proof worth testing.

RTI Connext is therefore not "Milestone 5" by default. It becomes justified when
interoperability with that implementation is an actual requirement.

## Engineering thesis

> **How do you know distributed state is still safe to act on when the network,
> producer, authority, identity, permissions, schema, timing or resource
> assumptions begin to fail?**

That is the systems problem this lab is designed to make visible.
