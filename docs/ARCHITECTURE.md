# Architecture notes

RemoteX is divided into a native data plane and a Go control plane. The boundaries are intended to keep latency-sensitive desktop work independent from account, fleet, and persistence concerns.

## Native data plane

The shared `core` library owns authentication clients, connection settings, signaling, WebRTC coordination, input messages, file-transfer framing, and cryptographic helpers.

The `host` application runs capture and control responsibilities on Windows. Its main subsystems cover desktop capture, damage tracking, frame rotation, encoder selection, input injection, clipboard synchronization, recording, backup, privacy mode, heartbeat, and service lifecycle.

The `viewer` is a Qt desktop application. It manages login and machine selection, remote-session rendering, input capture, transfer dialogs, chat, recordings, reports, diagnostics, groups, users, and update presentation.

The `updater` separates replacement and recovery behavior from the main host process. Release metadata, payload hashes, and Ed25519 signatures are validated before activation.

## Go control plane

The API is organized by domain rather than transport layer alone:

- `auth`: credentials, tokens, middleware, administrative challenges, and rate limiting
- `machines` and `machinegroups`: tenant-scoped fleet state and grouping
- `sessions`: authorization, lifecycle, metrics, and audit integration
- `signaling`: WebSocket session coordination
- `relay`: authenticated TCP fallback for session traffic
- `turn`: short-lived TURN credential delivery
- `recordings` and `backup`: metadata and object-storage workflows
- `releases`, `installer`, and `update`: controlled distribution lifecycle
- `admin`, `audit`, `reports`, and `users`: operational governance

PostgreSQL is the system of record. Redis supports rate limiting, presence, and cross-node coordination. Large payloads are kept in S3-compatible object storage rather than the relational database.

## Security boundaries

- Tenant identity is carried through authenticated request context and enforced in service and repository operations.
- Administrative authentication uses a separate challenge flow and supports TOTP.
- Signaling authorization accepts intended credentials without placing JWTs in URLs.
- Relay tickets are scoped and time-bound.
- Logs redact tokens, passwords, session identifiers, and other sensitive fields.
- Release private keys are deliberately absent from source control.
- Portfolio endpoints use the reserved `.example` namespace and cannot resolve to production infrastructure.

## Reliability choices

- Capture, encoding, and publication track frame freshness to avoid building unbounded latency.
- Heartbeats and explicit session closure reconcile transient disconnects.
- Update workflows validate artifacts before replacement and monitor post-update health.
- Redis-backed signaling supports multiple nodes while keeping a single-node development path.
- Contract and integration tests exercise failure behavior in addition to happy paths.

