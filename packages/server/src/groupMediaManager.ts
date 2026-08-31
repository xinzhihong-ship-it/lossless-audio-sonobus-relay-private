import { createHash } from "node:crypto";
import { spawn, type ChildProcess } from "node:child_process";
import { ingestVideoPath, type MediaMtxAdmin, videoRoom } from "./mediaMtx.js";

export type ActiveVideoGroup = { group: string; groupPassword: string };

export type GroupMediaManagerConfig = {
  bridgeBinary: string;
  ffmpegBinary: string;
  connectionHost: string;
  connectionPort: number;
  relayHost: string;
  relayPort: number;
  mediaMtxRtspHost: string;
  mediaMtxRtspPort: number;
  muxerUsername: string;
  muxerPassword: string;
};

type Worker = ActiveVideoGroup & {
  bridge?: ChildProcess;
  ffmpeg?: ChildProcess;
  audioBackpressured: boolean;
  lastError?: string;
};

export class GroupMediaManager {
  private readonly workers = new Map<string, Worker>();
  private readonly pollTimer: NodeJS.Timeout;
  private polling = false;
  private stopped = false;

  constructor(private readonly config: GroupMediaManagerConfig, private readonly mediaMtx: MediaMtxAdmin) {
    this.pollTimer = setInterval(() => void this.poll(), 500);
    this.pollTimer.unref();
  }

  reconcile(groups: ActiveVideoGroup[]): void {
    if (this.stopped) return;
    const wanted = new Map(groups.map((group) => [group.group, group]));
    for (const [name, worker] of this.workers) {
      const next = wanted.get(name);
      if (!next || next.groupPassword !== worker.groupPassword) {
        this.stopWorker(worker);
        this.workers.delete(name);
      }
    }
    for (const group of wanted.values()) {
      if (this.workers.has(group.group)) continue;
      const worker: Worker = { ...group, audioBackpressured: false };
      this.workers.set(group.group, worker);
      this.startBridge(worker);
    }
    void this.poll();
  }

  status(): Array<{ group: string; audioBridge: boolean; muxer: boolean; error?: string }> {
    return [...this.workers.values()].map((worker) => ({
      group: worker.group,
      audioBridge: Boolean(worker.bridge),
      muxer: Boolean(worker.ffmpeg),
      error: worker.lastError
    }));
  }

  close(): void {
    this.stopped = true;
    clearInterval(this.pollTimer);
    for (const worker of this.workers.values()) this.stopWorker(worker);
    this.workers.clear();
  }

  private startBridge(worker: Worker): void {
    if (this.stopped || this.workers.get(worker.group) !== worker) return;
    const username = `media-mix-${createHash("sha256").update(worker.group).digest("hex").slice(0, 12)}`;
    const bridge = spawn(this.config.bridgeBinary, [], {
      env: {
        ...process.env,
        BRIDGE_CONNECTION_HOST: this.config.connectionHost,
        BRIDGE_CONNECTION_PORT: String(this.config.connectionPort),
        BRIDGE_GROUP: worker.group,
        BRIDGE_GROUP_PASSWORD: worker.groupPassword,
        BRIDGE_USERNAME: username,
        BRIDGE_ADMIN_PORT: "0",
        BRIDGE_RELAY_HOST: this.config.relayHost,
        BRIDGE_RELAY_PORT: String(this.config.relayPort),
        BRIDGE_RAW_STDOUT: "1"
      },
      stdio: ["ignore", "pipe", "pipe"]
    });
    worker.bridge = bridge;
    bridge.stdout!.on("data", (chunk: Buffer) => {
      const input = worker.ffmpeg?.stdin;
      if (!input?.writable || worker.audioBackpressured) return;
      worker.audioBackpressured = !input.write(chunk);
      if (worker.audioBackpressured) input.once("drain", () => { worker.audioBackpressured = false; });
    });
    bridge.stderr!.on("data", (chunk: Buffer) => { worker.lastError = lastLine(chunk.toString()); });
    bridge.on("error", (error) => { worker.lastError = error.message; });
    bridge.on("close", () => {
      if (worker.bridge === bridge) worker.bridge = undefined;
      if (!this.stopped && this.workers.get(worker.group) === worker) {
        setTimeout(() => this.startBridge(worker), 1000).unref();
      }
    });
  }

  private async poll(): Promise<void> {
    if (this.stopped || this.polling || !this.workers.size) return;
    this.polling = true;
    try {
      const ready = new Set((await this.mediaMtx.paths()).filter((path) => path.ready).map((path) => path.name));
      for (const worker of this.workers.values()) {
        const ingestPath = ingestVideoPath(worker.group);
        if (ingestPath && ready.has(ingestPath) && !worker.ffmpeg) this.startFfmpeg(worker);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      for (const worker of this.workers.values()) worker.lastError = message;
    } finally {
      this.polling = false;
    }
  }

  private startFfmpeg(worker: Worker): void {
    if (this.stopped || this.workers.get(worker.group) !== worker || worker.ffmpeg) return;
    const ffmpeg = spawn(this.config.ffmpegBinary, buildFfmpegArgs(this.config, worker.group), {
      env: process.env,
      stdio: ["pipe", "ignore", "pipe"]
    });
    worker.ffmpeg = ffmpeg;
    worker.audioBackpressured = false;
    ffmpeg.stderr!.on("data", (chunk: Buffer) => { worker.lastError = lastLine(chunk.toString()); });
    ffmpeg.on("error", (error) => { worker.lastError = error.message; });
    ffmpeg.on("close", () => {
      if (worker.ffmpeg === ffmpeg) worker.ffmpeg = undefined;
      worker.audioBackpressured = false;
    });
  }

  private stopWorker(worker: Worker): void {
    worker.ffmpeg?.kill("SIGTERM");
    worker.bridge?.kill("SIGTERM");
    worker.ffmpeg = undefined;
    worker.bridge = undefined;
  }
}

export function buildFfmpegArgs(config: GroupMediaManagerConfig, group: string): string[] {
  const room = videoRoom(group);
  const ingestPath = ingestVideoPath(group);
  if (!room || !ingestPath) throw new Error("Invalid video group.");
  const inputUrl = rtspUrl(config, ingestPath);
  const outputUrl = rtspUrl(config, room);
  return [
    "-hide_banner", "-loglevel", "warning", "-nostdin",
    "-rtsp_transport", "tcp", "-fflags", "nobuffer", "-flags", "low_delay",
    "-analyzeduration", "0", "-probesize", "32768", "-use_wallclock_as_timestamps", "1", "-i", inputUrl,
    "-thread_queue_size", "1024", "-use_wallclock_as_timestamps", "1", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "pipe:0",
    "-map", "0:v:0", "-map", "1:a:0",
    "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
    "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-x264-params", "repeat-headers=1",
    "-c:a", "libopus", "-b:a", "160k", "-application", "lowdelay", "-frame_duration", "10", "-af", "aresample=async=1:first_pts=0",
    "-max_interleave_delta", "0", "-muxdelay", "0", "-f", "rtsp", "-rtsp_transport", "tcp", outputUrl
  ];
}

function rtspUrl(config: GroupMediaManagerConfig, path: string): string {
  const user = encodeURIComponent(config.muxerUsername);
  const password = encodeURIComponent(config.muxerPassword);
  const encodedPath = path.split("/").map(encodeURIComponent).join("/");
  return `rtsp://${user}:${password}@${config.mediaMtxRtspHost}:${config.mediaMtxRtspPort}/${encodedPath}`;
}

function lastLine(value: string): string {
  return value.trim().split(/\r?\n/).at(-1)?.slice(0, 500) ?? "";
}
