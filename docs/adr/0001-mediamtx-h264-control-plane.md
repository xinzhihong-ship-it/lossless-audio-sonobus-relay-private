# ADR 0001: MediaMTX H.264 delivery and administrator camera control

- Status: Accepted
- Date: 2026-08-09

## Context

The initial self-hosted video path encoded each camera frame as JPEG, split it across custom SBV1 UDP packets, reassembled it in Node, and forwarded JPEG frames over WebSocket. That design is simple but cannot efficiently sustain clear 60 FPS video or provide mature congestion handling, browser WebRTC playback, or a native OBS media URL.

No earlier ADR constrains the video transport, so this decision does not conflict with an existing ADR.

## Decision

- Remove the custom JPEG/SBV1 transport.
- Use a crash-isolated Windows/macOS helper for camera capture and low-latency hardware H.264 encoding.
- On Windows, select the highest-throughput color source currently exposed by `SharedReadOnly`; on macOS, keep selecting the highest verified 60 FPS mode. Publish the source unchanged by default, with persisted administrator output caps that may only downscale, drop frames, or reduce bitrate.
- Publish video-only RTSP to self-hosted MediaMTX.
- Use MediaMTX WebRTC/WHEP for browsers and RTSP for OBS.
- Keep one stable public H.264 + Opus stream per SonoBus group and at most one active camera publisher in that group.
- Dynamically join each live SonoBus group with a headless bridge, mix it to 48 kHz stereo, encode the public mix as Opus 160 kbps, and leave native SonoBus audio/UDP 9000 unchanged.
- Make the authenticated administrator page the only application control for camera on/off and camera selection after one-time local OS permission/pairing. Persist the administrator's desired state across reconnects, while always showing local capture indication.
- Keep capture, encoding, mixing, and network work outside the real-time audio callback.

## Consequences

- Public ports remain TCP 19090, with UDP 19091 for WebRTC ICE, and add TCP 19092 for RTSP.
- MediaMTX and a dynamic audio muxing/bridge layer become required server components.
- Client installers must include a checksum-pinned helper/runtime.
- One-camera-per-group limits upload bandwidth and keeps one browser/OBS URL, but does not provide a multi-camera mosaic.
- The sub-500 ms target must be measured end to end; it is not guaranteed by configuration alone.
