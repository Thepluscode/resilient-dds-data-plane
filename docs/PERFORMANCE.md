# Performance Validation

The anomaly path should be cheap enough that diagnostics do not become the data-plane bottleneck.

## Microbenchmark

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/resilientdds_benchmark
```

The benchmark processes one million valid samples from one source through the sequence, freshness and schema checks.

This is **not** a DDS throughput benchmark. It isolates the application-level trustworthiness checks so transport latency, serialization and discovery do not hide their cost.

## Measured end-to-end DDS latency

Publisher and subscriber as separate processes, 1200 samples at 200 Hz,
`periodic_telemetry` (RELIABLE / TRANSIENT_LOCAL / KEEP_LAST 32), inside one
container on Docker Desktop for macOS:

```text
samples_received = 1200 / 1200
min  =   36 us
mean =  189 us
p99  =  625 us
max  = 3898 us
```

Exported as `rdtf_e2e_latency_{min,mean,p99,max}_us`. Derived from the
publisher's `source_timestamp`, so it is valid only while both endpoints share a
clock; across hosts this requires NTP or PTP. Measured in a Linux VM, so the
tail is not representative of bare metal.

## Live DDS performance gate

Still outstanding for Milestone 2:

- publication rate;
- receive rate;
- p50/p95/p99 end-to-end latency;
- jitter;
- CPU and RSS for writer/reader;
- sample loss;
- deadline misses;
- recovery after induced partition;
- behavior at configured resource limits.

Do not quote a headline "messages/sec" number without payload size, QoS profile, host topology and test duration.
