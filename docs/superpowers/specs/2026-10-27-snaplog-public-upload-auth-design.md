# SnapLog Regional Telemetry And Public Upload Auth Design

## Goal

Move the SnapLog batch upload transport from the logged-in Bearer endpoints to the public HMAC endpoints while preserving the existing event payload and client behavior. The only removed account dependency is the token returned by `/api/account` and `notify.account.changed`.

## Commit Scope

This is the consolidated design for the regional SnapLog and PostHog telemetry feature. Besides public upload authentication, the commit covers regional endpoint selection, GUI event instrumentation, operation helpers, and PostHog routing. Native telemetry call sites include application lifecycle in `GUI_App`, import/export/slicing outcomes in `Plater`, first-run privacy handling in `WebGuideDialog`, and SSWCP operations. `Preferences` owns the user-facing region switch and refreshes the live SnapLog configuration when that region changes.

The regional configuration exposes only PostHog projects that currently have events routed to them: CN Orca, International Orca, and International Model Lab. Project identifiers for International Client and International Model Community remain supported by the transport, but their keys are not injected until an event route uses them.

## Auth Contract

All SnapLog batch lifecycle requests use the public paths:

```text
POST {snaplog}/api/log/public/upload/create
POST {snaplog}/api/log/public/upload/completed
POST {snaplog}/api/log/public/upload/cancel
```

Every request carries:

```text
X-Client-Type: Orca
X-Client-Id: <clientId>
X-Timestamp: <current unix milliseconds>
X-Nonce: <fresh 128-bit random value as 32 lowercase hex chars>
X-Sign: <lowercase HMAC-SHA256 hex>
```

The signature input remains exactly:

```text
clientType + clientId + timestamp + nonce
```

The segments are concatenated with no separator. The signing secret is injected through CMake and is not a user credential. The client does not send `Authorization`, decrypt the gateway account token, or otherwise use that token for SnapLog.

## Account Identity

`GUI_App` continues mirroring `/api/account` and `notify.account.changed`, and continues pushing `userid` into `SnapLogClient`. `userid` remains a payload identity field, not request authentication. An empty `userid` represents the anonymous state and does not block admission or upload.

SnapLog admission no longer requires a token or user id. Nickname, account, email, and token remain excluded from SnapLog payloads.

## Unchanged Behavior

- Event registry routes are unchanged.
- Event names, levels, ext fields, NDJSON rendering, privacy handling, and PostHog payloads are unchanged.
- Queue capacity, rate limiting, consent handling, spool rotation, retry, cancel, and completed behavior are unchanged.
- The spool lock and multi-instance behavior are unchanged.
- Regional SnapLog base URL selection is unchanged.

Upload error handling keeps terminal handling for authentication or signature failures. A terminal public-auth failure retains sealed files and stops further create attempts for the session, matching the current Bearer behavior without treating account-token refresh as a recovery mechanism.

## Security And Configuration

The public signing secret is read from the `SNAP_LOG_PUBLIC_HMAC_SECRET` environment variable on every CMake configure and passed through a target-scoped compile definition, matching the SnapLog PostHog key handling. The variable is not cached and has no source fallback. Tests inject their own secret through `SnapLogConfig`.

Nonce generation uses OpenSSL `RAND_bytes` for 16 bytes and encodes them as 32 lowercase hex characters. Each lifecycle request receives a fresh nonce. Tests may inject a deterministic nonce generator.

## Testing

Focused tests cover:

- All three lifecycle builders target the public paths.
- All three carry the five public headers and no `Authorization` header.
- `X-Sign` matches the documented concatenation and known test vector.
- Each request receives a fresh nonce.
- Empty signing secret produces no request.
- SnapLog admission and upload accept both anonymous (`userid` empty) and signed-in events without an account token.
- Existing event fields and PostHog routing remain unchanged.
- Source hygiene rejects logged-in `/api/log/upload/` paths and direct `Authorization` use in SnapLog batch transport.
