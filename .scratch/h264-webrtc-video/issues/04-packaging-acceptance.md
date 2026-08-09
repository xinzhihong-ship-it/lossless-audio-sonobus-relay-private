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
- Initial deploy bundle CI `31310828230` passed at `55f0324`; SHA-256 `9625e73bf898fa50ba6c3c9a9b740325517cf3f0bc10c7aeaaf4ea9c21ad9e0a`.
- The macOS `.pkg` is intentionally an unsigned test installer: no Developer ID Installer identity or notarization credentials are configured.
- Pending: physical Windows/macOS camera and DAW-host acceptance, real SonoBus group-mix feedback/sync checks, public cold-start latency, and signed/notarized macOS distribution.

### 2026-08-09 public deployment acceptance
- Deployed MediaMTX 1.20.0, Node server and Caddy to `82.156.228.183`; retained `/opt/lossless-audio.backup-20260809-124121` and a validated PostgreSQL dump under `/opt/lossless-audio-safety-20260809-124121`.
- Verified public TCP `19090/19092`, UDP listeners `9000/10998/19091`, loopback-only `19094`, no host `9997`, MediaMTX API `401` unauthenticated / `200` authenticated, and server restart recovery.
- Public FFprobe confirmed H.264 Constrained Baseline 1280x720@60 plus Opus 48 kHz stereo. Five isolated Chromium viewers each advanced 597-600 frames in the same 10-second window while OBS 32.2.1 read the RTSP URL; MediaMTX recorded five WebRTC readers and one RTSP reader.
- The selected WebRTC candidate was `82.156.228.183:19091/UDP`; a 30-second stable window decoded 1,800 video frames with zero new packet loss, drops, freezes, audio concealment or silent concealment.
- Final CI passed: macOS `31313473919`, Windows `31313473931`, ASIO `31313473940`, Linux client `31313473983`, and server `31314751416`. The Windows jobs silently installed, checked and uninstalled the generated Inno packages.
- The latest server Artifact SHA-256 is `93881d389ca22f6c404aa6733062a4fd3311a311915cf498805380a9bafa86ca`; all 64 non-secret runtime files matched production by content.
- Final package SHA-256: macOS pkg `e9a0fcd744f8d7f4dd3a5166f4a00137e728373721a509933e9f9d2a0823cb78`; Windows installer `49036306a7c27769a871d7e2dd70a2651fef2925e65851911cf331dc3d7c7530`; ASIO installer `b0bf38de5a6bc76e85dbd8403df364650e28e0877b7312a56be978ae700cb96f`; Linux deb `784144902aee4beef4abfc29353b2bbd10663877161fb1c1444badb983d65b48`; Linux tar `4b4b613d2a1eba6981167b7ea4a8bd37007b5cc6b6e924411a058e92ee390c93`.
- The temporary deployment public key was removed, login with its private key was denied, and local key files were deleted.

### 2026-08-09 admin UI and Windows pairing follow-up
- Redesigned the admin connection view into five operational columns with responsive two-/one-column rows; camera state now distinguishes unpaired, offline, no-device and ready states. Quiet 3-second refreshes avoid unchanged DOM rebuilds and preserve focused controls.
- Browser checks at 1365, 1024 and 640 px reported zero body/table/camera overflow. Login, auto-refresh, logout races, forced refresh ordering, focus retention, same-token relogin and stale pairing-secret suppression passed; independent review found no remaining issue.
- Server bundle CI `31320621529` passed at private commit `3bac4f4`; Artifact SHA-256 `4e5386f016e11b77532c21109b4fb27d4eda24bc39bcacca31cd8aab7ac6473c`. Production now serves the new admin UI; backup `/opt/lossless-audio-safety-ui-20260809-232310`, rollback image `sonobus-server:rollback-ui-20260809-232310`, app source SHA-256 `9e513116d697fc98938a37bf85ca1ecbf27c9daa0b24c343619e6fc7f637116c`.
- Production diagnosis showed `xiaomo / Administrator` audio online while its exact matching video control had never received a poll. Client commit `964d1f0` now auto-saves a complete `SBPAIR1...` value and keeps signed control polling alive to report a missing FFmpeg runtime instead of appearing offline.
- Final Windows CI `31322388606` and ASIO CI `31322388590` passed. Installer SHA-256: normal `4c8212ecea9aa239adeb6458655fdafc0009c6e4e86521d53e49ed866fd29572`; ASIO `602dbcda442697e680935eab27f9755f17e39d95665943b94bff2b607244745f`.
- Temporary deployment access was removed and its private key denied, then local key files and tunnel were deleted.

### 2026-08-10 SharedReadOnly capture and automatic enrollment
- Windows capture now uses `MediaCaptureSharingMode::SharedReadOnly -> MediaFrameReader -> NV12 -> FFmpeg stdin`; FFmpeg no longer opens DirectShow. It never requests exclusive control, never silently switches away from the administrator-selected source-group ID, and rejects measured rates below 59 FPS instead of duplicating frames to claim 60 FPS.
- A busy, disconnected or sub-60 shared stream is returned in signed client status and shown in the admin row while the device selector remains available. Device enumeration and capture restart use independent exponential retry capped at 30 seconds; successful measured 60 FPS resets the backoff.
- Removed the public/admin plaintext pairing-key route and all client pairing-code input. An authenticated SonoBus session now supplies a high-entropy enrollment capability; the public HTTP exchange contains only HMAC-authenticated requests and signed pending/approved state. Explicit admin approval creates the pairing, and the derived key is saved to Windows Credential Manager/macOS Keychain, never the DAW project.
- Final CI passed at `3b7d222`: Windows `31329583477`, ASIO `31329583475`, macOS `31329583472`, Linux client `31329583557`; parent Server bundle `31328679592` passed at `6e72190`. Both Windows jobs passed MSVC build, UTF-8/helper-protocol CTest, three-format helper checks, silent installer verification, installed helper `--list`, and silent uninstall.
- Final SHA-256: Windows installer `389fab54f98384fcb4d9ee928530f5f1437e47fadb1a889b9f6d6afbac250dc2`; ASIO `f6740390aa252502aa9998a32c08a19c855f2e6c1312450cce22615fb539bfbb`; macOS pkg `4a53dc805701b4129a5a24db1fa4557e879fc0d04d184ba22e6c387d7a46e61d`; Linux deb `f3283473df4a9f7a100f669fd051a74f60e5a5eea725ea2c45eb459fed47f810`; Server bundle `bc95f33ed2d6bbdf3ee6d2a9d7e25ac274911d38ab162f4c13c0e170dba30591`.
- Production deployment is healthy with safety copy `/opt/lossless-audio-safety-sharedcam-20260810-022551`, PostgreSQL dump, and rollback images `sonobus-server:rollback-sharedcam-20260810-022551` / `sonobus-connection-server:rollback-sharedcam-20260810-022551`. Checks: health/admin loopback `200`, public admin `404`, invalid enrollment `403`, removed pairing route `404`, connection-admin unauthenticated `401` / authenticated `200`, no host `18098/9997`, `19094` loopback-only.
- Production admin data at 1365/1024/640 px had `scrollWidth == viewport`, zero overflowing elements, and camera controls wrapped from 390 px to 582 px without clipping. Current legacy client `xiaomo / Administrator` remains online with zero reported cameras, as expected until the new installer is used.
- The temporary deployment key was removed, reuse was denied, and local key/JWT files were deleted. Pending physical acceptance: install the new Windows package and verify a real camera shared concurrently with Teams/OBS/Camera, exclusive-owner error/recovery, actual 60 FPS source mode, H.264 publication, group audio sync and cold-start latency.
