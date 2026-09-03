import assert from "node:assert/strict";
import test from "node:test";
import { buildFfmpegArgs, type GroupMediaManagerConfig } from "./groupMediaManager.js";

const config: GroupMediaManagerConfig = {
  bridgeBinary: "/bridge",
  ffmpegBinary: "/ffmpeg",
  connectionHost: "connection-server",
  connectionPort: 10998,
  relayHost: "server",
  relayPort: 9000,
  mediaMtxRtspHost: "mediamtx",
  mediaMtxRtspPort: 8554,
  muxerUsername: "media-muxer",
  muxerPassword: "secret:with@chars"
};

test("group muxer passes client H264 through and adds low-latency Opus on stable RTSP paths", () => {
  const args = buildFfmpegArgs(config, "studio");
  const joined = args.join(" ");
  assert.match(joined, /-c:v copy/);
  assert.doesNotMatch(joined, /libx264/);
  assert.match(joined, /-c:a libopus -b:a 160k -application lowdelay -frame_duration 10/);
  assert.match(joined, /-max_interleave_delta 1000000/);
  assert.doesNotMatch(joined, /rw_timeout/);
  assert.deepEqual(args.filter((value) => value === "-timeout"), ["-timeout"]);
  assert.deepEqual(args.filter((value) => value === "10000000"), ["10000000"]);
  assert.doesNotMatch(joined, /use_wallclock_as_timestamps/);
  assert.match(joined, /rtsp:\/\/media-muxer:secret%3Awith%40chars@mediamtx:8554\/ingest\/SB_studio/);
  assert.match(joined, /rtsp:\/\/media-muxer:secret%3Awith%40chars@mediamtx:8554\/SB_studio$/);
  assert.doesNotMatch(joined, /jpeg|mjpeg/i);
});
