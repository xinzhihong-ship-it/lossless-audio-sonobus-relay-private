# Integrate the helper with SonoBus

Type: task
Status: resolved
Blocked by: 02

Replace `VideoRelayClient` with signed control polling and helper lifecycle management, persist only `pairingId` in projects, restore administrator state from the server, expose real video metrics, and keep Standalone/VST3/AU behavior aligned.

## Done when

- Joining/leaving a group starts/stops control polling correctly.
- Reload restores the pairing ID and secure-store key; projects contain no camera/device/auto-connect control.
- UI reports actual codec/resolution/FPS/bitrate.
- Camera removal, helper failure and network loss reconnect safely.

## Comments

### 2026-08-09 resolution
- Standalone, VST3, VST3 Instrument and AU build with the same signed control/helper path.
- HMAC polling, secure-store scoping, helper restart and server-desired-state restoration are implemented and covered by builds/tests.
