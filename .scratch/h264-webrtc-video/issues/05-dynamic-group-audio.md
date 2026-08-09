# Add dynamic SonoBus group mixing

Type: task
Status: open
Blocked by: 01

Replace the single fixed-group web bridge with on-demand headless group subscriptions. Mix each live group to 48 kHz stereo PCM, feed the MediaMTX muxer, encode Opus at 160 kbps, and publish one stable H.264 + Opus path per group without changing the native SonoBus audio relay.

## Done when

- Every active group can independently start/stop its bridge.
- The public stream contains the complete group mix without audio feedback.
- Empty groups tear down their bridge/muxer and release resources.
- OBS and browser play synchronized group audio.

## Comments
