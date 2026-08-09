# Add dynamic SonoBus group mixing

Type: task
Status: claimed
Blocked by: 01

Replace the single fixed-group web bridge with on-demand headless group subscriptions. Mix each live group to 48 kHz stereo PCM, feed the MediaMTX muxer, encode Opus at 160 kbps, and publish one stable H.264 + Opus path per group without changing the native SonoBus audio relay.

## Done when

- Every active group can independently start/stop its bridge.
- The public stream contains the complete group mix without audio feedback.
- Empty groups tear down their bridge/muxer and release resources.
- OBS and browser play synchronized group audio.

## Comments

### 2026-08-09 resolution
- `GroupMediaManager` creates one headless bridge and muxer per active group, emits 48 kHz stereo PCM, and publishes Opus 160 kbps beside copied H.264.
- Lifecycle and stale-worker tests pass; local FFprobe confirmed H.264 Baseline plus Opus stereo on the stable public path.
- Pending resolution evidence: simultaneous browser/OBS playback with confirmed synchronization and no audio feedback.
- Public synthetic-source acceptance proved simultaneous H.264+Opus playback in five browsers and OBS with zero loss/concealment in a 30-second stable window, but it did not exercise a real SonoBus group mix.
