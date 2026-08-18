# Validation Plan

The lab is only valuable if it can falsify its own claims.

## V1 — Normal periodic telemetry

**Setup:** 10 Hz writer, reliable/transient-local profile.

**Expected:**
- no anomalies;
- health remains healthy;
- zero deadline misses.

## V2 — Missing sequence

**Injection:** publish 40 then 44.

**Expected:**
- one `sequence_gap` event;
- `missing_samples = 3`;
- gap counter increments exactly once.

## V3 — Duplicate and out-of-order

**Injection:** publish 50, 51, 51, 49.

**Expected:**
- duplicate event for second 51;
- out-of-order event for 49;
- last accepted sequence must not regress.

## V4 — Stale but delivered

**Injection:** deliver a sample whose source timestamp is 900 ms old under a 250 ms freshness budget.

**Expected:** `stale` anomaly even if DDS delivery is successful.

## V5 — Clock skew

**Injection:** source timestamp >50 ms ahead of receiver clock.

**Expected:** `future_timestamp` anomaly.

## V6 — Deadline miss

**Injection:** pause a periodic writer longer than the configured deadline.

**Expected:** middleware callback plus degraded source health.

## V7 — Liveliness loss

**Injection:** stop manual liveliness assertion / terminate writer.

**Expected:** source transitions to `lost` and liveliness-loss counter increments.

## V8 — Late joiner

**Setup:** writer publishes state before reader starts using reliable + transient-local durability.

**Expected:** late reader receives retained recent state.

**Negative control:** switch reader/writer to volatile and verify historical samples are not expected.

## V9 — QoS incompatibility

**Injection:** configure reader/writer with incompatible requested/offered QoS.

**Expected:** explicit incompatible-QoS health event; system must not report healthy merely because discovery occurred.

## V10 — Schema drift

**Injection:** schema version 2 sample to consumer expecting version 1.

**Expected:** `schema_mismatch` evidence. No silent interpretation.

## Acceptance gate

Do not call the project "robust" until live DDS tests demonstrate V1–V10 and the results are captured in CI or a reproducible test script.
