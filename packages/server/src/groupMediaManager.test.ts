import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import test from "node:test";
import type { ChildProcess } from "node:child_process";
import {
  buildFfmpegArgs,
  GroupMediaManager,
  type GroupMediaManagerConfig,
  type GroupMediaProcessSpawner
} from "./groupMediaManager.js";

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

class FakeChild extends EventEmitter {
  readonly stdin = new PassThrough();
  readonly stdout = new PassThrough();
  readonly stderr = new PassThrough();
  exitCode: number | null = null;
  signalCode: NodeJS.Signals | null = null;
  readonly signals: NodeJS.Signals[] = [];

  constructor(private readonly closeAfterKill = true) {
    super();
  }

  kill(signal: NodeJS.Signals = "SIGTERM"): boolean {
    this.signals.push(signal);
    if (signal === "SIGKILL") {
      this.signalCode = signal;
      if (this.closeAfterKill) queueMicrotask(() => this.emit("close", null, signal));
    }
    return true;
  }

  finishClose(): void {
    this.emit("close", this.exitCode, this.signalCode);
  }
}

async function waitForChildren(children: FakeChild[], count: number): Promise<void> {
  for (let attempt = 0; attempt < 100; ++attempt) {
    if (children.length >= count) return;
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  assert.fail(`expected at least ${count} children, got ${children.length}`);
}

test("group manager does not overlap a replacement while a stopped muxer is still alive", async () => {
  const children: FakeChild[] = [];
  const spawnProcess: GroupMediaProcessSpawner = (() => {
    const child = new FakeChild();
    children.push(child);
    return child as unknown as ChildProcess;
  }) as GroupMediaProcessSpawner;
  const mediaMtx = { paths: async () => [{ name: "ingest/SB_studio", ready: true }] };
  const manager = new GroupMediaManager(config, mediaMtx, { spawnProcess, stopGraceMs: 0 });

  manager.reconcile([{ group: "studio", groupPassword: "old" }]);
  await waitForChildren(children, 2);
  assert.equal(children.length, 2, "bridge and muxer should start for the first desired group");

  manager.reconcile([]);
  manager.reconcile([{ group: "studio", groupPassword: "new" }]);
  assert.equal(children.length, 2, "replacement must wait for both old children to close");
  assert.deepEqual(children.map((child) => child.signals), [["SIGTERM"], ["SIGTERM"]]);

  await waitForChildren(children, 4);
  assert.equal(children.length, 4, "replacement bridge and muxer should start after old children close");
  assert.deepEqual(children.slice(0, 2).map((child) => child.signals), [["SIGTERM", "SIGKILL"], ["SIGTERM", "SIGKILL"]]);
  assert.deepEqual(manager.status(), [{ group: "studio", audioBridge: true, muxer: true, error: undefined }]);

  await manager.close();
});

test("synchronous bridge spawn failure does not leave a phantom worker", async () => {
  const spawnProcess: GroupMediaProcessSpawner = (() => {
    throw new Error("synthetic spawn failure");
  }) as GroupMediaProcessSpawner;
  const manager = new GroupMediaManager(config, { paths: async () => [] }, { spawnProcess });

  manager.reconcile([{ group: "studio", groupPassword: "old" }]);
  assert.deepEqual(manager.status(), []);
  await manager.close();
});

test("group manager waits for close after a child reports its signal", async () => {
  const children: FakeChild[] = [];
  const spawnProcess: GroupMediaProcessSpawner = (() => {
    const child = new FakeChild(children.length >= 2);
    children.push(child);
    return child as unknown as ChildProcess;
  }) as GroupMediaProcessSpawner;
  const mediaMtx = { paths: async () => [{ name: "ingest/SB_studio", ready: true }] };
  const manager = new GroupMediaManager(config, mediaMtx, { spawnProcess, stopGraceMs: 0 });

  manager.reconcile([{ group: "studio", groupPassword: "old" }]);
  await waitForChildren(children, 2);
  manager.reconcile([{ group: "studio", groupPassword: "new" }]);
  await new Promise((resolve) => setTimeout(resolve, 5));
  assert.deepEqual(children.slice(0, 2).map((child) => child.signals), [["SIGTERM", "SIGKILL"], ["SIGTERM", "SIGKILL"]]);
  assert.equal(children.length, 2, "signalCode must not count as close");

  children[0].finishClose();
  children[1].finishClose();
  await waitForChildren(children, 4);
  assert.equal(children.length, 4);
  await manager.close();
});
