import { randomUUID } from "node:crypto";

export type VideoPresenceKind = "video-publisher" | "video-viewer";

export type VideoPresenceConnection = {
  type: VideoPresenceKind;
  sessionId: string;
  roomId: string;
  videoRoom: string;
  group: string;
  user: string;
  camera?: string;
  address?: string;
  port?: number;
  createdAt: string;
  lastSeenAt: string;
  online: boolean;
};

type VideoPresenceRecord = Omit<VideoPresenceConnection, "online">;

const DEFAULT_ONLINE_TTL_MS = 15_000;
const DEFAULT_RETENTION_MS = 5 * 60_000;

export type VideoPresenceHeartbeat = {
  sessionId?: string;
  type?: VideoPresenceKind;
  group: string;
  user: string;
  camera?: string;
  address?: string;
  port?: number;
};

export class VideoPresenceRegistry {
  private readonly records = new Map<string, VideoPresenceRecord>();

  constructor(
    private readonly onlineTtlMs = DEFAULT_ONLINE_TTL_MS,
    private readonly retentionMs = DEFAULT_RETENTION_MS
  ) {}

  heartbeat(input: VideoPresenceHeartbeat): VideoPresenceConnection {
    const now = new Date().toISOString();
    const sessionId = input.sessionId || randomUUID();
    const existing = this.records.get(sessionId);
    const record: VideoPresenceRecord = {
      type: input.type ?? existing?.type ?? "video-publisher",
      sessionId,
      roomId: input.group,
      videoRoom: input.group.startsWith("SB_") ? input.group : `SB_${input.group}`,
      group: input.group,
      user: input.user,
      camera: input.camera ?? existing?.camera,
      address: input.address ?? existing?.address,
      port: input.port ?? existing?.port,
      createdAt: existing?.createdAt ?? now,
      lastSeenAt: now
    };
    this.records.set(sessionId, record);
    return this.toConnection(record, Date.now());
  }

  remove(sessionId: string): boolean {
    return this.records.delete(sessionId);
  }

  connections(now = Date.now()): VideoPresenceConnection[] {
    for (const [sessionId, record] of this.records) {
      if (now - Date.parse(record.lastSeenAt) > this.retentionMs) {
        this.records.delete(sessionId);
      }
    }

    return [...this.records.values()]
      .map((record) => this.toConnection(record, now))
      .sort((left, right) => right.lastSeenAt.localeCompare(left.lastSeenAt));
  }

  kick(sessionId: string): number {
    return this.remove(sessionId) ? 1 : 0;
  }

  private toConnection(record: VideoPresenceRecord, now: number): VideoPresenceConnection {
    return {
      ...record,
      online: now - Date.parse(record.lastSeenAt) <= this.onlineTtlMs
    };
  }
}
