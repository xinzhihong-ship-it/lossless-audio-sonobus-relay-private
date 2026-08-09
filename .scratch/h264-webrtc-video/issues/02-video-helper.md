# Build the H.264 publisher helper

Type: task
Status: resolved
Blocked by: 01

Create the Windows/macOS companion process that selects the highest permitted camera source, performs low-latency H.264 hardware encoding, applies only downward administrator output limits, publishes video-only RTSP, reconnects, and exposes status/control IPC.

## Done when

- No work runs in the SonoBus audio callback.
- No silent 640×360 fallback exists.
- Encoder reports actual resolution, FPS, bitrate, codec, and errors.
- A helper crash does not crash the DAW.

## Comments

### 2026-08-09 resolution
- The crash-isolated companion reports capture/output metrics and restarts independently of audio processing. Windows uses the highest-throughput current `SharedReadOnly` source and only downscales/drops frames when an administrator limit requires it; macOS keeps probing real 60 FPS modes.
- macOS completed a real 1080p60 VideoToolbox probe; Windows CI now asserts DirectShow, H.264 encoder and RTSP support in the bundled runtime.
