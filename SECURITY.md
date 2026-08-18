# Security Policy

## Scope

This is an engineering portfolio lab, not a certified safety- or security-critical product.

## Current security posture

- Core parsing/anomaly logic is dependency-free and built with strict compiler warnings.
- CI runs unit tests and sanitizer builds for the core.
- The live DDS path is exercised through real RTPS and network-degradation scenarios.
- Milestone 3 adds DDS Security configuration for PKI-DH authentication, Access-Permissions authorization, and AES-GCM-GMAC cryptographic protection.
- Milestone 3 test credentials are generated ephemerally; no CA or participant private key is committed.
- Negative security scenarios cover an untrusted identity, a trusted-but-unauthorized publisher, and an insecure peer.
- A plaintext pcap positive control is paired with the encrypted-wire test so an empty/blind capture cannot masquerade as encryption evidence.
- The diagnostic JSONL log is **not tamper-evident**.
- Do not expose the lab to untrusted networks or treat it as production middleware.

See `docs/DDS_SECURITY.md` for the security model, test matrix, and explicit non-claims.

## Vulnerability reporting

Do not open a public issue containing credentials, exploit payloads that expose third-party systems, or private infrastructure details. For this portfolio repository, report reproducible project-local defects without including live secrets.

## Remaining hardening

1. Certificate expiry, rotation, and revocation tests.
2. Resource-limit and malformed-payload testing.
3. Tamper-evident evidence chain where required.
4. Dependency/CVE scanning for the selected DDS runtime.
5. RTI Connext security/interoperability validation when that SDK is available.
