# Milestone 3 — DDS Security

## Status

**Implemented and runtime-validated on Fast DDS 2.14.6.**

Validation anchor:

- branch commit: `c2b0c2e126e298ad2d492aef3e576ff174827072`;
- GitHub Actions run: `32178914685`;
- evidence artifact digest: `sha256:c9143f22bce757a82aa9ffd19aa17c9a548b9b9aee5f269fff0be459be574773`;
- security matrix: **6 passed, 0 failed**.

This is lab evidence, not a certification claim.

## Purpose

Milestones 1 and 2 establish that the data plane can move real RTPS traffic and expose when state becomes unsafe under timing, network, producer, schema and consumer failures.

Milestone 3 asks a separate question:

> Can the same data plane reject untrusted or unauthorized participants and protect application telemetry on the wire without weakening the existing failure evidence?

## Middleware baseline

The security/regression image pins **Fast DDS 2.14.6** and builds it from source with `SECURITY=ON`. This replaces the earlier Ubuntu package path that supplied Fast DDS 2.11.x for the live DDS container.

Fast DDS remains isolated behind `src/dds_transport.cpp`. The application-facing API accepts a `DdsSecurityConfig`; authentication/access-control/crypto properties are applied inside the vendor adapter.

## Security controls

When `--security-dir` is supplied, a participant is configured with:

- `builtin.PKI-DH` participant authentication;
- signed DDS governance and permissions documents;
- `builtin.Access-Permissions` authorization;
- topic-level publish/subscribe grants for `SystemTelemetry`;
- `builtin.AES-GCM-GMAC` cryptographic protection;
- governance that forbids unauthenticated participants and requests encrypted discovery, liveliness, RTPS, metadata and topic data.

Credentials are externally supplied file paths. The C++ library does not generate credentials, embed private keys or silently fall back to an insecure profile when a requested security file is missing.

## Disposable CI PKI

`scripts/security/generate_test_pki.sh` creates a two-day test PKI at runtime:

- trusted identity CA;
- separate rogue identity CA;
- permissions CA;
- trusted publisher identity;
- trusted subscriber identity;
- trusted-but-unauthorized publisher identity;
- untrusted publisher identity;
- signed governance document;
- role-specific signed permissions documents.

The script verifies every signed policy artifact before deleting the CA private keys. Generated credentials live only in the test workspace and are not intended for source control.

## Runtime security matrix

The successful GitHub Actions artifact recorded:

| Scenario | Result | Observed evidence |
|---|---|---|
| Secure baseline | PASS | trusted/authorized peers matched; 300 application samples received |
| Untrusted identity | PASS | publisher signed by rogue identity CA; 0 application samples received |
| Unauthorized writer | PASS | trusted identity with subscribe-only grant; DataWriter creation rejected, publisher exit code 1 |
| Insecure peer | PASS | secure governance rejected unauthenticated peer; 0 application samples received |
| Plaintext capture control | PASS | unique application marker visible 300 times in UDP pcap |
| Encrypted payload capture | PASS | secure peers exchanged 300 samples; same marker visible 0 times in UDP pcap |

### Secure-path latency observed in this CI run

For the 300-sample secure baseline:

```text
min     159 us
p50     285 us
p95     390 us
p99     412 us
p99.9   535 us
max     535 us
mean    285.883 us
```

These are one-run CI measurements, not a latency SLA or statistical tail claim.

## Why the wire test has two controls

A statement such as "the secret marker was absent from the encrypted pcap" is weak by itself. The capture might simply be looking at the wrong interface or seeing no application traffic.

The harness therefore performs a paired test using the same marker:

```text
plaintext run  -> 300 marker occurrences in pcap
secure run     ->   0 marker occurrences in pcap
```

Both runs must also move application samples successfully.

The first Milestone 3 run exposed a harness defect rather than a DDS defect. The pcap contained the plaintext marker, but `strings | grep -q` executed under `pipefail`; `grep -q` exited on its first match, `strings` received SIGPIPE, and the positive control was falsely reported as failed. The harness now searches the binary pcap directly with `grep -aFq`, preserving the strict positive and negative controls.

## What the packet capture proves — and what it does not

The secure test proves that the unique **application source marker** present in plaintext RTPS traffic is absent from the captured secure traffic while samples are still delivered.

It does **not** prove that every byte on the wire is opaque. Certificate authority and certificate identity strings may be observable during DDS Security authentication/handshake traffic. Do not describe this result as "no metadata is visible" or "the whole network trace is unreadable."

## Existing reliability behavior remains visible

The security workflow reruns the existing RTPS failure scenarios and `tc netem` matrix before the security cases. The same commit therefore passed the reliability/network regression gate under Fast DDS 2.14.6, rather than proving security in an isolated toy executable.

One expected lifecycle detail remains visible in the secure baseline: after the finite publisher exits cleanly, the subscriber reports writer unmatch and later deadline misses while it remains alive. That is post-publisher silence, not a failure of authentication or encryption.

## Explicit non-claims

Milestone 3 does not establish:

- product security certification;
- safety certification;
- production certificate issuance or HSM-backed private keys;
- certificate expiry/revocation behavior;
- zero-downtime certificate/permission rotation;
- resistance to arbitrary malformed or hostile RTPS traffic;
- tamper-evident diagnostic JSONL;
- RTI Connext security interoperability;
- Fast DDS 3.x compatibility.

## Next security hardening

Security work should only continue where it closes a real operational failure mode:

1. certificate expiry/revocation and permission-rotation behavior;
2. credential rollover without an unsafe availability window;
3. malformed/hostile RTPS and resource-exhaustion tests;
4. dependency/CVE scanning as a release gate;
5. tamper-evident evidence only if the target use case requires it.
