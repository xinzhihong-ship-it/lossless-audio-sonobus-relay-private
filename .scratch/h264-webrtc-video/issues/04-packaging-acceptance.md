# Package and run acceptance tests

Type: task
Status: open
Blocked by: 01, 03

Package the helper/runtime in Windows and macOS installers, build all CI artifacts, deploy the server, and run quality/latency/concurrency/OBS acceptance tests.

## Done when

- Installed clients work without separately installing an encoder runtime.
- Browser end-to-end latency is below 500 ms in the acceptance setup.
- Highest available 60 FPS mode is reported and used.
- Five viewers and OBS Media Source play simultaneously without audio glitches.
- Temporary deployment credentials are removed.

## Comments

### 2026-08-09 local media acceptance
- Pinned universal FFmpeg 7.1.1 enumerated `OBS Virtual Camera` as `1920x1080@[60,60]fps` and completed a 30-frame VideoToolbox probe.
- Full local path produced MediaMTX `H264 Baseline 1920x1080 + Opus 48 kHz stereo`; FFprobe confirmed both tracks.
- Five simultaneous Chromium WebRTC sessions remained registered as five MediaMTX readers on the same public path.
- A 720p60 frame-embedded timestamp test measured 100/100 warm samples below 500 ms: median 280 ms, p95 293 ms, max 302 ms.
- macOS Standalone, VST3, VST3 Instrument and AU targets compiled locally with nested universal camera helpers.
- Linux server CI `31309925330` succeeded and produced the H.264 deployment bundle.
- Pending: latest Windows/ASIO/macOS/Linux-client CI, remote deployment, physical camera/DAW audio-glitch checks, and OBS UI acceptance.
