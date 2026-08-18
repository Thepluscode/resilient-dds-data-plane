#!/usr/bin/env python3
"""Turn a subscriber health timeline into failure-detection and recovery timings.

A partition test that stops at "health went LOST, PASS" is half a test. The
operationally interesting numbers are how fast the loss of trust is detected and
how long after restoration the state is trustworthy again.
"""
import re
import sys

log_path = sys.argv[1]
partition_abs_ms, restore_abs_ms = int(sys.argv[2]), int(sys.argv[3])

with open(log_path) as handle:
    text = handle.read()

epoch = re.search(r"EPOCH_MS=(\d+)", text)
if not epoch:
    print("\n## Partition and recovery\n\n    no EPOCH_MS anchor in log; cannot align clocks")
    sys.exit(0)
epoch_ms = int(epoch.group(1))

# Convert the harness's wall-clock fault marks into the subscriber's own frame.
partition_ms = partition_abs_ms - epoch_ms
restore_ms = restore_abs_ms - epoch_ms

events = [
    (int(m.group(1)), m.group(2), m.group(3))
    for m in re.finditer(r"HEALTH_AT t_ms=(\d+) state=(\w+) reason=(\w+)", text)
]

print("\n## Partition and recovery\n")
print(f"    partition applied at   t={partition_ms} ms")
print(f"    partition removed at   t={restore_ms} ms\n")
if not events:
    print("    no health timeline captured")
    sys.exit(0)

for t, state, reason in events:
    mark = "   <-- partition" if partition_ms <= t < partition_ms + 300 else ""
    print(f"    t={t:>6} ms  {state:<9} {reason}{mark}")


def first_after(floor, predicate):
    return next((t for t, state, reason in events if t >= floor and predicate(state, reason)), None)


degraded = first_after(partition_ms, lambda s, r: s == "degraded")
lost = first_after(partition_ms, lambda s, r: s == "lost")
healthy = first_after(restore_ms, lambda s, r: s == "healthy")

fmt = lambda v, base: f"{v - base} ms" if v is not None else "not observed"
print()
print(f"    time_to_unsafe (degraded)  = {fmt(degraded, partition_ms)}")
print(f"    time_to_lost               = {fmt(lost, partition_ms)}")
print(f"    recovery_to_healthy        = {fmt(healthy, restore_ms)}")

if degraded is not None and lost is not None:
    print(
        f"\n    The consumer's freshness budget was breached {lost - degraded} ms before\n"
        "    DDS declared the writer lost. The application safety boundary is\n"
        "    crossed first, so the two signals are complementary, not redundant."
    )
