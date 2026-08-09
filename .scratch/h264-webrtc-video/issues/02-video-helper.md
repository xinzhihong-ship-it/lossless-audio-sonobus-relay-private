# Build the H.264 publisher helper

Type: task
Status: resolved
Blocked by: 01

Create the Windows/macOS companion process that enumerates 60 FPS camera modes, chooses the highest resolution, performs low-latency H.264 hardware encoding, publishes video-only RTSP, reconnects, and exposes localhost status/control IPC.

## Done when

- No work runs in the SonoBus audio callback.
- No silent 640×360 fallback exists.
- Encoder reports actual resolution, FPS, bitrate, codec, and errors.
- A helper crash does not crash the DAW.

## Comments

### 2026-08-09 resolution
- The crash-isolated FFmpeg companion enumerates and probes real 60 FPS modes, reports metrics, and restarts independently of audio processing.
- macOS completed a real 1080p60 VideoToolbox probe; Windows CI now asserts DirectShow, H.264 encoder and RTSP support in the bundled runtime.
