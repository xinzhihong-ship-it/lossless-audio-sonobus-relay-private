# Package and run acceptance tests

Type: task
Status: claimed
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
- OBS 32.2.1 opened `rtsp://127.0.0.1:28559/SB_test` as a real `ffmpeg_source`; MediaMTX reported an active RTSP reader with increasing bytes.
- The fixed-source round passed all five CI workflows at `1c64dac`; downloaded macOS, Windows, ASIO and Linux installers were inspected for expected bundles, helpers and licenses.
- Latest server bundle CI `31310828230` passed at `55f0324`; SHA-256 `9625e73bf898fa50ba6c3c9a9b740325517cf3f0bc10c7aeaaf4ea9c21ad9e0a`.
- The macOS `.pkg` is intentionally an unsigned test installer: no Developer ID Installer identity or notarization credentials are configured.
- Pending: physical Windows/macOS camera and DAW-host acceptance, real SonoBus group-mix feedback/sync checks, public cold-start latency, and signed/notarized macOS distribution.

### 2026-08-09 public deployment acceptance
- Deployed MediaMTX 1.20.0, Node server and Caddy to `82.156.228.183`; retained `/opt/lossless-audio.backup-20260809-124121` and a validated PostgreSQL dump under `/opt/lossless-audio-safety-20260809-124121`.
- Verified public TCP `19090/19092`, UDP listeners `9000/10998/19091`, loopback-only `19094`, no host `9997`, MediaMTX API `401` unauthenticated / `200` authenticated, and server restart recovery.
- Public FFprobe confirmed H.264 Constrained Baseline 1280x720@60 plus Opus 48 kHz stereo. Five isolated Chromium viewers each advanced 597-600 frames in the same 10-second window while OBS 32.2.1 read the RTSP URL; MediaMTX recorded five WebRTC readers and one RTSP reader.
- The selected WebRTC candidate was `82.156.228.183:19091/UDP`; a 30-second stable window decoded 1,800 video frames with zero new packet loss, drops, freezes, audio concealment or silent concealment.
- Final CI passed: macOS `31313473919`, Windows `31313473931`, ASIO `31313473940`, Linux client `31313473983`, and server `31314751416`. The Windows jobs silently installed, checked and uninstalled the generated Inno packages.
- The latest server Artifact SHA-256 is `93881d389ca22f6c404aa6733062a4fd3311a311915cf498805380a9bafa86ca`; all 64 non-secret runtime files matched production by content.
- The temporary deployment public key was removed, login with its private key was denied, and local key files were deleted.
