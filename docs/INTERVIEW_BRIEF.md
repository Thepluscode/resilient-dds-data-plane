# Interview Brief

## 30-second explanation

I built a C++ DDS data-plane lab around a problem I kept seeing in distributed systems: receiving a message does not mean the state is safe to act on. The project uses DDS QoS for delivery contracts, then adds application-level freshness, sequence, schema and health checks. It can detect stale samples, gaps, duplicates, out-of-order data, deadline misses, writer liveliness loss and incompatible QoS, with metrics and anomaly evidence.

## Two things the lab actually taught me

**A clean shutdown is not a liveliness loss.** I expected killing the publisher
to fire `on_liveliness_changed`. It did not — a normal exit deletes the
participant, the reader unmatches, and `subscription_matched` drops to 0 with no
liveliness event at all. Only `SIGKILL`, which sends no goodbye message, leaves
the lease to expire. So a health model built on liveliness alone is blind to the
orderly case, and one built on matching alone is blind to the crash. You need
both, and the test has to kill the process hard or it proves nothing.

**Transient-local replay hands you stale state.** A reader joining three seconds
late got the full history from sequence 1 — and 25 of those samples immediately
breached the freshness budget. The middleware did exactly what it promised. The
application still had to decide that most of what it just received was not safe
to act on. That is the difference between a delivery contract and a
trustworthiness decision, and it is the reason the project exists.

**Loss becomes latency, not gaps.** I expected packet loss to show up as missing
samples. Under RELIABLE at 15% loss the application saw zero gaps and 82 stale
samples, with p50 latency at 249 ms against a 250 ms freshness budget —
retransmission recovered every sample and spent the entire time budget doing it.
The same impairment on BEST_EFFORT gave 31 gaps and 0 stale samples with
unchanged latency. So the QoS choice is not "reliable is safer": it is whether
this consumer would rather have old data or missing data, and only the
application knows that.

**The unsafe window is longer than the detection window.** During a partition the
consumer's freshness budget broke at +262 ms and DDS declared the writer lost at
+1005 ms. For a 20 ms control loop that is ~37 cycles of acting on state the
middleware still considered fine. That gap is why the freshness guard and the
liveliness callback are complementary rather than duplicated effort.

**A slow reader looks like nothing and breaks everything.** With no network fault
at all, a subscriber 2x slower than its publisher produced p99 latency over a
second and 93% stale samples, identically under both QoS profiles. No delivery
contract fixes a consumer slower than its producer, and a health model built
only on DDS callbacks reports nothing wrong.

## Why DDS instead of Kafka?

Kafka is excellent for durable ordered event streams and replay-oriented asynchronous systems. DDS is data-centric pub/sub aimed at low-latency distributed and real-time systems, with endpoint-level QoS contracts such as deadline, liveliness, durability and reliability. This project uses those semantics rather than treating DDS as a generic message bus.

## Why not rely only on RELIABLE QoS?

Because reliable delivery answers whether samples are delivered according to the middleware contract. It does not prove that a delivered value is fresh, semantically compatible or the state the application should currently act on.

## Why transient-local durability?

For state-oriented topics, a consumer that joins after a writer has started may need the latest retained values. Transient-local durability provides that late-joiner behavior while the writer remains the durability source. It is not a replacement for an archival event log.

## Why a vendor adapter?

The target role mentions RTI DDS, but my public implementation begins on an open-source DDS implementation. Keeping vendor APIs behind an adapter lets me demonstrate DDS concepts without falsely claiming production RTI experience and creates a clean path to a Connext implementation later.

## What I would add for production

- DDS Security with authenticated participants and access control;
- secure key/certificate lifecycle;
- bounded resource limits and memory profiling;
- network partition and packet-loss test harness;
- P99 latency/jitter baselines;
- deterministic time source strategy;
- schema compatibility tests using XTypes rules;
- signed/tamper-evident evidence where audit integrity is required;
- deployment-specific discovery configuration;
- RTI Connext compatibility backend if required by programme constraints.
