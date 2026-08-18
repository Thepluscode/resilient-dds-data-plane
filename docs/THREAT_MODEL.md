# Threat and Failure Model

This document separates **communication failure**, **data-quality failure**, and **adversarial risk**.

## Failure classes

### Availability
- writer crash;
- reader crash;
- discovery interruption;
- network partition;
- resource exhaustion;
- blocked writer caused by reliable-delivery pressure.

### Integrity / correctness
- duplicate sample;
- missing sequence;
- out-of-order delivery/application processing;
- stale state;
- source clock skew;
- schema mismatch;
- incompatible QoS configuration.

### Security extensions not yet implemented
- unauthorized participant discovery;
- topic-level unauthorized publish/subscribe;
- certificate/key compromise;
- malformed serialized payload;
- replay by an unauthorized actor;
- tampering with local diagnostic evidence.

## Current trust boundary

This repository currently assumes DDS transport peers are trusted for the dependency-free core demonstration. Do not market the current state as a secure DDS deployment. DDS Security integration is a separate milestone.
