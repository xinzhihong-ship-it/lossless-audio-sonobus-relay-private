# Integrate the helper with SonoBus

Type: task
Status: open
Blocked by: 02

Replace `VideoRelayClient` with helper lifecycle and IPC control, preserve camera selection and auto-connect state, expose real video metrics, and keep Standalone/VST3/AU behavior aligned.

## Done when

- Joining/leaving a group starts/stops the helper correctly.
- Reload restores the last camera and group behavior.
- UI reports actual codec/resolution/FPS/bitrate.
- Camera removal and network loss reconnect safely.

## Comments
