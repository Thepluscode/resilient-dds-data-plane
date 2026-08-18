# Pain-to-Feature Map

| Pain observed in complex systems | Consequence | Project response | Evidence |
|---|---|---|---|
| "The service is up, but data stopped" | silent bad decisions | deadline + liveliness health | health transition/counter |
| "We received the message, but it was old" | action on stale state | freshness budget | stale anomaly with age |
| "Some readings disappeared" | state discontinuity | sequence gap tracking | exact missing count |
| "A restart produced old/current confusion" | incorrect recovery | transient-local state + sequence guard | late-joiner test |
| "Two teams changed the model differently" | integration break | IDL + schema guard | schema mismatch evidence |
| "One QoS tweak broke discovery/matching" | hidden integration outage | named QoS contracts + incompatibility signal | health evidence |
| "We cannot explain what failed" | long integration/debug cycles | metrics + JSONL evidence | reproducible incident trail |
| "Vendor APIs leak through everything" | high switching cost | transport adapter boundary | core compiles without DDS |
