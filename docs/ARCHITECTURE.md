# Architecture

## Design goals

1. Detect bad distributed state, not merely transport errors.
2. Keep failure semantics deterministic and testable without a running DDS stack.
3. Treat QoS profiles as named application contracts.
4. Preserve vendor portability.
5. Generate evidence useful during integration and incident diagnosis.

## Components

### IDL model

`idl/SystemTelemetry.idl` defines keyed telemetry and health-event types. The telemetry type carries a monotonic sequence, source and ingest timestamps, health flags and a schema version.

### AnomalyDetector

Maintains per-source sequence state and performs:

- duplicate detection;
- missing-sequence detection;
- out-of-order detection;
- stale-data detection;
- clock-skew detection;
- schema-version checking.

A gap does not silently advance through missing values: the exact number of missing samples is recorded.

### HealthMonitor

Maintains a per-source operational state:

```text
HEALTHY -> DEGRADED -> LOST
   ^          |          |
   +----------+----------+
          fresh sample
```

Signals include application freshness, DDS deadline misses, liveliness loss and incompatible QoS.

### MetricsRegistry

Dependency-free metric sink using Prometheus text format. This keeps core tests small and permits a later HTTP exporter without coupling the domain layer to a web framework.

### AuditSink

Appends one JSON object per anomaly. This is not presented as a tamper-proof audit log; it is diagnostic evidence. A production extension would add hash chaining/signatures and controlled retention.

### DDS adapter

The adapter is the anti-corruption layer around vendor DDS APIs. Listener callbacks should translate middleware events into core health events.

## Data flow

```text
DDS callback
   |
   +--> middleware status ----------> HealthMonitor
   |
   +--> sample --> AnomalyDetector --> AuditSink
                         |
                         +-----------> MetricsRegistry
```

## Important boundaries

- DDS should enforce transport/data-distribution contracts.
- The application still owns semantic freshness and schema acceptance.
- A `RELIABLE` sample can still be stale.
- A live writer can still publish semantically invalid data.
- A durable late-joiner cache is not an event archive.
- Health and safety policy must be explicit above middleware delivery semantics.
