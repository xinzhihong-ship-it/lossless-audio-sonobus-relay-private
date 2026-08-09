# H.264 / WebRTC self-hosted video

Status: approved

## Problem

The current SBV1 JPEG-over-UDP relay is bandwidth-heavy, fragments every frame into many datagrams, has no codec congestion control, and cannot satisfy clear 60 FPS video with sub-500 ms latency. It must be removed rather than tuned down to a blurry resolution.

## Confirmed requirements

- Fully self-hosted; no VDO.Ninja or third-party runtime video service.
- Windows and macOS camera publishers in SonoBus Standalone, VST3, and AU builds.
- Windows selects the highest-throughput color source currently available through `SharedReadOnly`; macOS selects the highest verified 60 FPS mode. The administrator may set persisted output resolution/FPS/bitrate caps below the captured mode, never above it.
- End-to-end latency target: below 500 ms.
- 1–5 simultaneous viewers per group.
- Browser viewers install nothing.
- OBS supports a real Media Source URL on TCP 19092.
- Each group exposes one live camera selected by the administrator.
- The administrator page is the only camera on/off and device-selection control after one-time local camera authorization.
- The administrator's last camera state and device selection survive client reconnect/restart.
- The administrator page binds only to server `127.0.0.1:19094` and is reached through an SSH tunnel; public `19090` remains a viewer/control-poll entry point.
- The public WebRTC/RTSP stream contains the whole SonoBus group mix as Opus 48 kHz stereo at 160 kbps.
- SonoBus audio transport and UDP 9000 remain unchanged; capture, encoding, mixing, and networking stay outside the audio callback.

## Architecture

- Replace JPEG/SBV1 with H.264 low-latency encoding (`yuv420p`, no B-frames, one-second-or-shorter GOP).
- Run MediaMTX on the existing server.
- A selected publisher sends video-only H.264 to a private MediaMTX ingest path over RTSP.
- A dynamic headless SonoBus bridge joins every live group, mixes the group, and provides PCM to a server muxer.
- The server muxer copies H.264, encodes only the group mix to Opus, and publishes one stable public path per group.
- Browser reads the public stream through MediaMTX WebRTC/WHEP.
- OBS reads `rtsp://<server>:19092/SB_<group>` as a Media Source.
- Node remains the admin/auth/control surface, persists desired camera state, and derives stream status from MediaMTX's control API.
- Public ports: TCP 19090 for viewer/control-poll/WebRTC HTTP, UDP 19091 for WebRTC ICE media, TCP 19092 for RTSP publish/read; loopback-only TCP 19094 serves administration.

## Publisher strategy

Use a crash-isolated companion video process controlled by SonoBus, not encoder work inside the plugin process audio path. The helper reports device IDs/names, obeys authenticated administrator commands, selects the highest current shared source without taking exclusive ownership, uses the platform H.264 hardware encoder when available, applies only downward output limits, publishes video-only RTSP, reconnects, and reports capture plus output resolution/FPS/bitrate over IPC. Packaging must be self-contained and checksum-pinned. The operating system still performs first camera permission approval and the client always displays a visible capture indicator.

## Acceptance

1. No JPEG frame encoder, SBV1 packetizer, UDP JPEG assembler, or JPEG WebSocket viewer remains.
2. Windows publishes its highest current `SharedReadOnly` source by default; administrator limits can only reduce resolution, FPS, or bitrate. A supported macOS 60 FPS camera publishes its highest verified 60 FPS mode.
3. Only the administrator can start/stop a camera or select its device after one-time local authorization; the last administrator state restores after reconnect/restart.
4. Enabling another person in the same group stops the previous publisher, so the group has one live camera and one stable URL.
5. The public stream carries H.264 video plus the complete group mix as Opus 48 kHz stereo at 160 kbps.
6. Browser playback uses WebRTC and measures below 500 ms end-to-end latency in the acceptance setup.
7. Five concurrent browser viewers remain live without publisher audio glitches.
8. Admin shows each person's camera switch/device list, actual codec/resolution/FPS/bitrate, stream state, browser button, and OBS RTSP copy button.
9. OBS Media Source plays the copied RTSP URL with synchronized group audio.
10. Camera/network interruption reconnects without restarting the DAW and restores the administrator's desired state.
11. Windows, Windows ASIO, and macOS CI builds produce installable artifacts; server tests and deployment health checks pass.
