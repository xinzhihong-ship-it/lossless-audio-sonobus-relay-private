import { createHash } from "node:crypto";
import { spawn, type ChildProcess, type SpawnOptions } from "node:child_process";
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

export type GroupMediaProcessSpawner = (
  command: string,
  args: readonly string[],
  options: SpawnOptions
) => ChildProcess;

export type GroupMediaManagerOptions = {
  spawnProcess?: GroupMediaProcessSpawner;
  stopGraceMs?: number;
};

type Worker = ActiveVideoGroup & {
  bridge?: ChildProcess;
  ffmpeg?: ChildProcess;
  audioBackpressured: boolean;
  stopping: boolean;
  lastError?: string;
  lastWarnAt?: number;
};

export class GroupMediaManager {
  private readonly workers = new Map<string, Worker>();
  private readonly stoppingProcesses = new Map<string, Set<ChildProcess>>();
  private readonly stoppingWaiters = new Set<() => void>();
  private readonly retryTimers = new Map<string, NodeJS.Timeout>();
  private readonly desiredGroups = new Map<string, ActiveVideoGroup>();
  private readonly pollTimer: NodeJS.Timeout;
  private readonly spawnProcess: GroupMediaProcessSpawner;
  private readonly stopGraceMs: number;
  private polling = false;
  private stopped = false;

  constructor(
    private readonly config: GroupMediaManagerConfig,
    private readonly mediaMtx: MediaMtxAdmin,
    options: GroupMediaManagerOptions = {}
  ) {
    this.spawnProcess = options.spawnProcess ?? spawn;
    const stopGraceMs = options.stopGraceMs ?? 1000;
    this.stopGraceMs = Number.isFinite(stopGraceMs) ? Math.max(0, stopGraceMs) : 1000;
    this.pollTimer = setInterval(() => void this.poll(), 500);
    this.pollTimer.unref();
  }

  reconcile(groups: ActiveVideoGroup[]): void {
    if (this.stopped) return;
    const wanted = new Map(groups.map((group) => [group.group, group]));
    for (const [group, timer] of this.retryTimers) {
      if (!wanted.has(group)) {
        clearTimeout(timer);
        this.retryTimers.delete(group);
      }
    }
    this.desiredGroups.clear();
    for (const group of wanted.values()) this.desiredGroups.set(group.group, group);
    for (const [name, worker] of this.workers) {
      const next = wanted.get(name);
      if (!next || next.groupPassword !== worker.groupPassword) {
        this.workers.delete(name);
        this.stopWorker(worker);
      }
    }
    for (const group of wanted.values()) this.maybeStartWorker(group);
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

  async close(): Promise<void> {
    if (this.stopped) return;
    this.stopped = true;
    this.desiredGroups.clear();
    for (const timer of this.retryTimers.values()) clearTimeout(timer);
    this.retryTimers.clear();
    clearInterval(this.pollTimer);
    for (const worker of this.workers.values()) this.stopWorker(worker);
    this.workers.clear();

    if (!this.stoppingProcesses.size) return;
    let resolveWaiter!: () => void;
    const stopped = new Promise<void>((resolve) => {
      resolveWaiter = () => {
        this.stoppingWaiters.delete(resolveWaiter);
        resolve();
      };
      this.stoppingWaiters.add(resolveWaiter);
    });
    await stopped;
  }

  private maybeStartWorker(group: ActiveVideoGroup): void {
    if (this.stopped || this.workers.has(group.group) || this.stoppingProcesses.has(group.group)
        || this.retryTimers.has(group.group)) return;
    const worker: Worker = { ...group, audioBackpressured: false, stopping: false };
    this.workers.set(group.group, worker);
    try {
      this.startBridge(worker);
    } catch (error) {
      this.workers.delete(group.group);
      worker.stopping = true;
      const bridge = worker.bridge;
      worker.bridge = undefined;
      if (bridge) this.stopProcesses(group.group, [bridge]);
      else this.scheduleWorkerRetry(group.group);
      console.warn(`[groupMedia:${group.group}] bridge start failed: ${errorMessage(error)}`);
    }
  }

  private startBridge(worker: Worker): void {
    if (this.stopped || worker.stopping || this.workers.get(worker.group) !== worker) return;
    const username = `media-mix-${createHash("sha256").update(worker.group).digest("hex").slice(0, 12)}`;
    const bridge = this.spawnProcess(this.config.bridgeBinary, [], {
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
      if (!worker.stopping && !this.stopped && this.workers.get(worker.group) === worker) {
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
        if (ingestPath && ready.has(ingestPath) && !worker.stopping && !worker.ffmpeg) this.startFfmpeg(worker);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      for (const worker of this.workers.values()) worker.lastError = message;
    } finally {
      this.polling = false;
    }
  }

  private startFfmpeg(worker: Worker): void {
    if (this.stopped || worker.stopping || this.workers.get(worker.group) !== worker || worker.ffmpeg) return;
    const ffmpeg = this.spawnProcess(this.config.ffmpegBinary, buildFfmpegArgs(this.config, worker.group), {
      env: process.env,
      stdio: ["pipe", "ignore", "pipe"]
    });
    worker.ffmpeg = ffmpeg;
    worker.audioBackpressured = false;
    ffmpeg.stdin!.on("error", () => { worker.audioBackpressured = false; });
    ffmpeg.stderr!.on("data", (chunk: Buffer) => {
      const line = lastLine(chunk.toString());
      worker.lastError = line;
      const now = Date.now();
      if (line && now - (worker.lastWarnAt ?? 0) > 5000) {
        worker.lastWarnAt = now;
        console.warn(`[groupMedia:${worker.group}] ffmpeg: ${line}`);
      }
    });
    ffmpeg.on("error", (error) => { worker.lastError = error.message; });
    ffmpeg.on("close", (code, signal) => {
      if (worker.ffmpeg === ffmpeg) worker.ffmpeg = undefined;
      worker.audioBackpressured = false;
      console.warn(`[groupMedia:${worker.group}] muxer exited code=${code ?? "null"} signal=${signal ?? "none"} last=${worker.lastError ?? "none"}`);
    });
  }

  private stopWorker(worker: Worker): void {
    worker.stopping = true;
    const children = [worker.ffmpeg, worker.bridge].filter((child): child is ChildProcess => child !== undefined);
    worker.ffmpeg = undefined;
    worker.bridge = undefined;
    worker.audioBackpressured = false;
    this.stopProcesses(worker.group, children);
  }

  private stopProcesses(group: string, children: ChildProcess[]): void {
    // FFmpeg can stay blocked in an RTSP read or pipe write after SIGTERM. Keep a
    // per-group stopping barrier until every child emits close, then escalate once.
    if (!children.length) {
      this.maybeStartDesiredWorker(group);
      return;
    }

    let stopping = this.stoppingProcesses.get(group);
    if (!stopping) {
      stopping = new Set<ChildProcess>();
      this.stoppingProcesses.set(group, stopping);
    }
    for (const child of children) stopping.add(child);

    for (const child of children) {
      let forceTimer: NodeJS.Timeout | undefined;
      let finished = false;
      const finish = () => {
        if (finished) return;
        finished = true;
        if (forceTimer) clearTimeout(forceTimer);
        stopping!.delete(child);
        if (!stopping!.size) {
          this.stoppingProcesses.delete(group);
          this.maybeStartDesiredWorker(group);
          if (!this.stoppingProcesses.size) {
            const waiters = [...this.stoppingWaiters];
            for (const resolve of waiters) resolve();
          }
        }
      };
      child.once("close", finish);
      destroyChildStreams(child);
      if (processHasExited(child)) continue;
      let signaled = false;
      try {
        signaled = child.kill("SIGTERM");
      } catch {
        // The process may have exited between the state check and kill().
      }
      if (!signaled && !processHasExited(child)) {
        try { child.kill("SIGKILL"); } catch { /* best effort */ }
      }
      if (!finished) {
        forceTimer = setTimeout(() => {
          if (!processHasExited(child)) {
            try { child.kill("SIGKILL"); } catch { /* best effort */ }
          }
        }, this.stopGraceMs);
      }
    }
  }

  private maybeStartDesiredWorker(group: string): void {
    const desired = this.desiredGroups.get(group);
    if (!desired) return;
    this.maybeStartWorker(desired);
    void this.poll();
  }

  private scheduleWorkerRetry(group: string): void {
    if (this.stopped || this.retryTimers.has(group)) return;
    const timer = setTimeout(() => {
      this.retryTimers.delete(group);
      const desired = this.desiredGroups.get(group);
      if (desired) this.maybeStartWorker(desired);
    }, 1000);
    timer.unref();
    this.retryTimers.set(group, timer);
  }
}

function processHasExited(child: ChildProcess): boolean {
  return child.exitCode !== null || child.signalCode !== null;
}

function destroyChildStreams(child: ChildProcess): void {
  child.stdin?.destroy();
  child.stdout?.destroy();
  child.stderr?.destroy();
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
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
    // ponytail: 10s RTSP socket watchdog so a dead ingest (camera off) gets ffmpeg
    // killed; the poll loop restarts it clean once the path is ready again.
    "-timeout", "10000000",
    "-analyzeduration", "0", "-probesize", "32768", "-i", inputUrl,
    "-thread_queue_size", "1024", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "pipe:0",
    "-map", "0:v:0", "-map", "1:a:0",
    "-c:v", "copy",
    "-c:a", "libopus", "-b:a", "160k", "-application", "lowdelay", "-frame_duration", "10", "-af", "aresample=async=1:first_pts=0",
    "-max_interleave_delta", "1000000", "-muxdelay", "0", "-f", "rtsp", "-rtsp_transport", "tcp", outputUrl
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
