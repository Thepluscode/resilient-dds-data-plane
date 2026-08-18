# Milestone 3 — DDS Security

## Purpose

Milestones 1 and 2 establish that the data plane can move data over real RTPS and can expose when state becomes unsafe under timing, network, producer, schema, and consumer failures.

Milestone 3 asks a different question:

> Can the same data plane reject untrusted or unauthorized participants and protect telemetry on the wire without weakening the existing failure evidence?

This is a lab security boundary, not a certification claim.

## Middleware baseline

The security validation image pins **Fast DDS 2.14.6** and builds it from source with `SECURITY=ON`. The previous Ubuntu 24.04 package path supplied Fast DDS 2.11.x, which is retained nowhere in the security image.

The project keeps the Fast DDS API behind `src/dds_transport.cpp`; applications pass only a `DdsSecurityConfig`. That preserves the existing vendor seam for a later RTI Connext implementation.

## Security controls

When `--security-dir` is supplied, a participant is configured with:

- PKI-DH participant authentication;
- signed DDS governance and permissions documents;
- topic-level publish/subscribe authorization;
- AES-GCM-GMAC cryptographic protection;
- encrypted discovery, liveliness, RTPS, metadata, and `SystemTelemetry` data according to the generated governance policy.

Credentials are paths to externally managed files. The C++ library does not generate credentials or embed private key material.

## Disposable CI PKI

`scripts/security/generate_test_pki.sh` generates a two-day, disposable test PKI at runtime:

- trusted identity CA;
- separate rogue identity CA;
- permissions CA;
- trusted publisher and subscriber identities;
- a trusted-but-unauthorized publisher identity;
- an untrusted publisher identity;
- signed governance and role permissions.

The script verifies each S/MIME signature before deleting CA private keys. No generated credential is intended to be committed.

## Scenario matrix

`scripts/security/run_security_scenarios.sh` is designed to prove six things:

| Scenario | Required result |
|---|---|
| Secure baseline | Trusted, authorized publisher/subscriber exchange data |
| Untrusted identity | Rogue identity CA reaches zero application samples |
| Unauthorized writer | Trusted identity without publish permission cannot produce application data |
| Insecure peer | Governance that forbids unauthenticated participants rejects an insecure publisher |
| Plaintext capture control | A unique source marker is visible in a deliberately insecure UDP pcap |
| Encrypted payload capture | Secure peers exchange data while the same marker is absent from the UDP pcap |

The plaintext capture is a required **positive control**. Without it, a missing marker in the encrypted capture could simply mean the capture watched the wrong interface. The harness therefore captures on `any` and fails if the insecure control cannot see the marker.

## Evidence standard

A green compile is not security evidence. Milestone 3 is only considered validated when the GitHub Actions run produces the scenario matrix and packet-capture controls above.

Until that run is green, the code in this milestone should be described as **implemented, pending runtime validation**.

Even after a green run, this project does **not** claim:

- product security certification;
- safety certification;
- production-grade certificate lifecycle or revocation;
- HSM-backed private keys;
- resistance to all malformed RTPS traffic;
- RTI Connext interoperability;
- protection of the JSONL diagnostic log against tampering.

## Next security work after this gate

1. Certificate expiry and revocation behavior.
2. Permission rotation without unsafe availability gaps.
3. Malformed/hostile RTPS and resource-exhaustion testing.
4. Tamper-evident evidence where required.
5. Dependency/CVE scanning as a release gate.
6. RTI Connext backend and interoperability tests when the SDK is available.
