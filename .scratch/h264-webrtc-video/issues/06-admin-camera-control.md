# Add administrator-only camera control

Type: task
Status: open
Blocked by: 02, 03

Pair each client after one-time local camera authorization, report stable camera IDs/names, and expose per-person administrator controls. Persist the administrator's desired on/off state and device. Enforce one active publisher per group; switching people stops the previous publisher.

## Done when

- Non-admin HTTP/WebSocket clients cannot issue camera commands.
- The admin row shows switch, device selector, acknowledgement/error, actual resolution/FPS/bitrate, and visible capture state.
- Desired state restores after reconnect/restart.
- The client always shows OS/capture indication and never bypasses camera permission.

## Comments
