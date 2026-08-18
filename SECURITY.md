# Security Policy

## Scope

This is an engineering portfolio lab, not a certified safety- or security-critical product.

## Current security posture

- Core parsing/anomaly logic is dependency-free and built with strict compiler warnings.
- CI runs unit tests and sanitizer builds for the core.
- The live DDS path is exercised through real RTPS and network-degradation scenarios.
- The current security/regression image pins Fast DDS **2.14.6** built with `SECURITY=ON`.
- DDS Security is runtime-validated for PKI-DH authentication, Access-Permissions authorization and AES-GCM-GMAC protection.
- GitHub Actions run `32178914685` produced a **6/6 PASS** security matrix on commit `c2b0c2e`.
- Test credentials are generated ephemerally; no generated CA or participant private key is committed.
- Negative scenarios cover a rogue identity CA, a trusted-but-unauthorized publisher and an insecure peer.
- A plaintext pcap positive control is paired with the encrypted-wire test so an empty/blind capture cannot masquerade as encryption evidence.
- The successful pcap pair observed the application marker 300 times in plaintext traffic and 0 times in the secure capture while 300 secure samples were delivered.
- This does not imply every security-handshake field is opaque; certificate/CA identity material can still be observable.
- The diagnostic JSONL log is **not tamper-evident**.
- Do not expose the lab to untrusted networks or treat it as production middleware.

See `docs/DDS_SECURITY.md` for the security model, evidence boundary and explicit non-claims.

## Vulnerability reporting

Do not open a public issue containing credentials, exploit payloads that expose third-party systems, or private infrastructure details. For this portfolio repository, report reproducible project-local defects without including live secrets.

## Remaining hardening

1. Certificate expiry, rotation and revocation tests.
2. Zero-downtime permission/credential rollover behavior.
3. Resource-limit and malformed/hostile RTPS testing.
4. Dependency/CVE scanning for the selected DDS runtime.
5. Tamper-evident evidence chain only where the target use case requires it.
6. RTI Connext security/interoperability validation when that SDK is available.
