# Add MediaMTX H.264/WebRTC server

Type: task
Status: resolved

Replace the Node JPEG relay with MediaMTX deployment, WebRTC/WHEP browser playback, RTSP 19092, ICE UDP 19091, control-API health/status, and admin URL generation.

## Done when

- Compose health checks pass.
- JPEG/SBV1 server code is deleted.
- Browser and OBS URLs are generated from one room-path function.
- Server/API tests cover URLs and unavailable-stream behavior.

## Comments

### 2026-08-09 resolution
- MediaMTX 1.20.0 owns RTSP/WebRTC fan-out; JPEG/SBV1 relay code is removed.
- URL, auth, unavailable-stream and dynamic-path tests pass; latest server bundle CI `31314751416` passed at private commit `5909a08`.
- Production deployment on `82.156.228.183` passed health, API auth, public WebRTC/RTSP, loopback-admin, restart and five-viewer-plus-OBS checks.
