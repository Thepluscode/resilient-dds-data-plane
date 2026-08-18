# Network degradation matrix (50 Hz, 6s, UDP-only on lo)

| impairment | profile | recv | gaps | stale | dl_miss | p50_us | p99_us | max_us |
|---|---|---|---|---|---|---|---|---|
| baseline     | periodic_telemetry |    210 |     0 |     0 |     0 |       368 |      1029 |      1604 |
| loss_1pct    | periodic_telemetry |    211 |     0 |     2 |     1 |       392 |    246692 |    292468 |
| loss_15pct   | periodic_telemetry |    164 |     2 |    82 |     9 |    249249 |    751700 |    771628 |
| delay_50ms   | periodic_telemetry |    207 |     0 |     0 |     0 |     51394 |     55518 |     58606 |
| jitter_100ms | periodic_telemetry |    212 |     0 |     0 |     0 |     77402 |    149014 |    167321 |
| reorder_20pct | periodic_telemetry |    227 |     0 |     0 |     0 |     12032 |     20912 |     41069 |
| baseline     | high_rate_sensor |    211 |     0 |     0 |     0 |       330 |      1872 |      8950 |
| loss_1pct    | high_rate_sensor |    209 |     3 |     0 |     0 |       302 |      1001 |     12755 |
| loss_15pct   | high_rate_sensor |    171 |    31 |     0 |     9 |       324 |      1253 |      1288 |
| delay_50ms   | high_rate_sensor |    210 |     0 |     0 |     0 |     51086 |     54831 |     55094 |
| jitter_100ms | high_rate_sensor |    113 |    61 |     0 |    45 |     19394 |    105845 |    106693 |
| reorder_20pct | high_rate_sensor |    225 |     0 |     0 |     0 |     12110 |     13073 |     23037 |

## Partition and recovery

    partition applied at   t=7015 ms
    partition removed at   t=16046 ms

    t=     0 ms  lost      no_data
    t=  1017 ms  healthy   none
    t=  7277 ms  degraded  stale_data   <-- partition
    t=  8020 ms  lost      liveliness_lost
    t= 16091 ms  healthy   none

    time_to_unsafe (degraded)  = 262 ms
    time_to_lost               = 1005 ms
    recovery_to_healthy        = 45 ms

    The consumer's freshness budget was breached 743 ms before
    DDS declared the writer lost. The application safety boundary is
    crossed first, so the two signals are complementary, not redundant.
| slow_reader  | periodic_telemetry |   1580 |    22 |  1466 |     - |         - |   1138910 | lost=1382 |
| slow_reader  | high_rate_sensor |   1557 |    21 |  1450 |     - |         - |   1156869 | lost=1365 |
