# Security Policy

## Scope

This is an engineering portfolio lab, not a certified safety- or security-critical product.

## Current security posture

- Core parsing/anomaly logic is dependency-free and built with strict compiler warnings.
- CI runs unit tests and sanitizer builds.
- The diagnostic JSONL log is **not tamper-evident**.
- DDS Security authentication/access-control is **not yet implemented**.
- Do not expose the demo to untrusted networks or treat it as production middleware.

## Planned hardening

1. DDS Security participant authentication.
2. Topic-level governance and permissions.
3. Certificate rotation and revocation tests.
4. Resource-limit and malformed-payload testing.
5. Tamper-evident evidence chain where required.
6. Dependency/CVE scanning for the selected DDS runtime.
