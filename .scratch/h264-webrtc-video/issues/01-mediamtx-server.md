# Add MediaMTX H.264/WebRTC server

Type: task
Status: claimed

Replace the Node JPEG relay with MediaMTX deployment, WebRTC/WHEP browser playback, RTSP 19092, ICE UDP 19091, control-API health/status, and admin URL generation.

## Done when

- Compose health checks pass.
- JPEG/SBV1 server code is deleted.
- Browser and OBS URLs are generated from one room-path function.
- Server/API tests cover URLs and unavailable-stream behavior.

## Comments
