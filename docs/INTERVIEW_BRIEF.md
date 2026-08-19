# Interview Brief

## 30-second explanation

I built a C++17 DDS reliability lab around one question: **when distributed
communication still appears healthy, is the state actually safe to act on?**
The project uses Fast DDS 2.14.6 with IDL-generated types and real RTPS, then
adds application-level freshness, sequence, schema and health checks. I tested
packet loss, partitions, late joiners, writer death, QoS mismatch, DDS Security,
primary/standby authority, bounded writer resources and fan-out isolation using
real multi-process scenarios and CI-backed evidence.

## 90-second version

The important part is not that I can publish and subscribe. The project is built
to falsify assumptions.

Under 15% packet loss, RELIABLE delivery can convert loss into retransmission
latency and stale state rather than visible gaps, while BEST_EFFORT exposes
missing samples but stays timely. During a partition, the application's
freshness budget can be breached hundreds of milliseconds before DDS declares
the writer lost, which means liveliness and freshness are complementary signals.
A late-joining TRANSIENT_LOCAL reader can also receive history successfully and
still receive state that is already too stale to use.

On the producer side, I separated two bounded-resource failure modes:
immediate `RETCODE_OUT_OF_RESOURCES` when the allocation pool is exhausted, and
bounded `RETCODE_TIMEOUT` when a RELIABLE writer can allocate a change but
cannot insert it into full history before `max_blocking_time`. I also compared
`KEEP_LAST` with bounded `KEEP_ALL`: the former can report successful writes
while the reader observes gaps, while the latter exposes overload to the
producer through backpressure.

The testing discipline became part of the project itself: controls are
mutation-tested, absence claims have positive controls, and the merge gate now
keys workflow evidence to the exact PR head SHA rather than trusting stale PR
check summaries.

## Strongest observed findings

- **Silent keyed-instance loss:** with the default Fast DDS instance limit, only
  10 of 32 keyed instances were delivered. Raising and consistently sizing the
  resource limits restored all 32.
- **Reliability can spend the time budget:** packet loss under RELIABLE can show
  up as stale data and high latency instead of application gaps.
- **Application unsafe before middleware lost:** partition tests exposed a
  several-hundred-millisecond window where state was already stale before DDS
  liveliness declared the writer lost.
- **Late join does not imply safe replay:** TRANSIENT_LOCAL history can be
  delivered correctly and still fail the freshness invariant immediately.
- **Authority is measurable:** EXCLUSIVE ownership with primary/standby writer
  strengths produced hard-failure failover under 110 ms across observed runs,
  with the first visible standby sample inside the freshness budget.
- **Writer overload has different failure paths:** the lab separately observed
  fast allocation exhaustion and bounded reliable-history timeout/recovery.
- **Fan-out isolation held in the tested shape:** eight healthy readers showed
  zero gaps; freezing one reader did not measurably degrade the others; a hot
  key did not make unrelated keys stale.
- **Healthy-looking tests can be wrong:** multiple controls only failed when the
  mechanism was deliberately mutated or the asserted condition was measured
  directly rather than inferred from an output proxy.

## Three testing lessons worth discussing

### 1. Measure the condition, not a proxy

The fan-out fault-isolation control originally inferred "the reader was frozen"
from sample counts. That was invalid with a RELIABLE TRANSIENT_LOCAL writer:
a resumed reader could drain retained history and finish with a higher count
than readers that were never stalled. The corrected control reads
`/proc/<pid>/stat` and requires process state `T`, which directly proves the
`SIGSTOP` condition.

### 2. Mutation testing catches thresholds that look sensible on paper

A 1 ms blocking threshold passed because ordinary write scheduling could already
exceed it, and a hot-key control passed because round-robin ordering gave one
extra sample. Both looked plausible in review and both failed once their
mechanisms were removed. The assertions were then tightened to distinguish the
real effect from noise.

### 3. A CI gate must bind evidence to the exact state being merged

The merge gate originally consumed PR-level checks that could span multiple
heads. A stale failure could block a good head and, in the dangerous direction,
a stale success could admit a broken one. The corrected gate resolves the
current head SHA, considers only runs for that SHA and aborts when there are no
matching runs.

The general rule is: **a correct check against the wrong state is still a false
control.**

## Why DDS instead of Kafka?

Kafka is excellent for durable ordered event streams, replay and asynchronous
processing. DDS is data-centric pub/sub aimed at low-latency and real-time
distribution, with endpoint QoS contracts such as deadline, liveliness,
durability, ownership, reliability and resource limits. This project uses those
semantics rather than treating DDS as a generic message bus.

## Why not rely only on RELIABLE QoS?

Because RELIABLE answers a middleware delivery contract. It does not prove that
a delivered value is fresh, semantically compatible, authorized, or the state
the application should currently act on.

## Why transient-local durability?

For state-oriented topics, a consumer joining after a writer starts may need
recent state immediately. TRANSIENT_LOCAL provides that late-join behavior while
the writer is the durability source. The lab demonstrates the important
limitation: successful replay still needs an application freshness verdict.

## Have you used RTI Connext?

No. This implementation uses Fast DDS 2.14.6 and does not claim RTI Connext
production experience. Vendor code is isolated behind a pimpl/adapter boundary
so a Connext implementation could target the same application-facing seam if a
programme requires it.

## What is already proven, not future work

- live DDS/RTPS processes and IDL code generation;
- QoS mapping and middleware callbacks;
- `tc netem` packet-loss, delay, jitter, reordering and partition scenarios;
- freshness, sequence and schema guards;
- DDS Security authentication, signed permissions and encrypted application
  traffic with paired packet-capture controls;
- EXCLUSIVE ownership primary/standby failover;
- bounded resource exhaustion, write timeout and recovery;
- `KEEP_LAST` versus bounded `KEEP_ALL` overload behavior;
- fan-out, frozen-reader isolation, hot-key isolation and keyed instance limits;
- CI, sanitizers for the core path, mutation-tested controls and exact-head
  merge gating.

## What I would still not claim

- RTI Connext implementation or interoperability;
- safety or security certification;
- Fast DDS 3.x compatibility;
- production certificate rotation/HSM lifecycle;
- arbitrary hostile/malformed RTPS resistance;
- statistical tail-latency guarantees across hardware populations;
- large-payload or deterministic allocator guarantees;
- multi-host fan-out/NIC/switch contention results;
- production split-brain/election protocol;
- sanitizer coverage of every live DDS transport thread.

## What would you do next?

For this portfolio repository, the engineering roadmap is frozen. The next
highest-value work is to use the evidence: record the demo, discuss the findings
in interviews and let a real role or programme expose the next missing proof.
RTI Connext would be a justified next experiment only if interoperability with
that implementation becomes an actual requirement.
