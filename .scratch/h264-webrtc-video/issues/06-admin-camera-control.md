# Add administrator-only camera control

Type: task
Status: resolved
Blocked by: 02, 03

Pair each client after one-time local camera authorization, report stable camera IDs/names, and expose per-person administrator controls. Persist the administrator's desired on/off state and device. Enforce one active publisher per group; switching people stops the previous publisher.

## Done when

- Non-admin HTTP/WebSocket clients cannot issue camera commands.
- The admin row shows switch, device selector, acknowledgement/error, actual resolution/FPS/bitrate, and visible capture state.
- Desired state restores after reconnect/restart.
- The client always shows OS/capture indication and never bypasses camera permission.

## Comments

### 2026-08-09 resolution
- Loopback-only admin controls persist device/on state, enforce one enabled camera per group in application and PostgreSQL, and restore state after reconnect.
- One-time secure pairing, HMAC replay protection, dynamic publisher credentials and `web-bridge`/`media-mix-*` system-account protection are covered by server tests.

### 2026-08-10 automatic enrollment and explicit shared-camera selection
- Replaced visible pairing codes with authenticated pending enrollment and explicit administrator approval. Pairing material is derived on each side, never returned by public HTTP, stored in Credential Manager/Keychain, and excluded from DAW project state.
- Administrators must select an exact reported camera before enabling capture; server validation rejects an empty device. Missing, busy or disconnected states stay attached to that selected ID, with no fallback device and 1-30 second retry. Persisted output caps may only reduce the selected source's resolution, FPS, or bitrate.
- Production responsive checks passed at 1365/1024/640 px with no horizontal overflow. The old pairing route returns `404`, invalid enrollment returns `403`, and current legacy clients correctly remain in the no-camera state until upgraded.
