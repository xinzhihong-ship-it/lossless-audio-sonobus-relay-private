export type MediaMtxTrack = {
  codec?: string;
  codecProps?: {
    width?: number;
    height?: number;
    sampleRate?: number;
    channelCount?: number;
  };
};

export type MediaMtxPath = {
  name: string;
  ready?: boolean;
  online?: boolean;
  bytesReceived?: number;
  bytesSent?: number;
  source?: { id?: string; type?: string } | null;
  readers?: Array<{ id?: string; type?: string }>;
  tracks?: string[];
  tracks2?: MediaMtxTrack[];
};

export type MediaMtxInternalUser = {
  user: string;
  pass?: string;
  ips: string[];
  permissions: Array<{ action: "publish" | "read" | "playback" | "api" | "metrics" | "pprof"; path?: string }>;
};

export interface MediaMtxAdmin {
  paths(): Promise<MediaMtxPath[]>;
  setInternalUsers?(users: MediaMtxInternalUser[]): Promise<void>;
}

export class HttpMediaMtxAdmin implements MediaMtxAdmin {
  constructor(
    private readonly baseUrl: string,
    private readonly username = "",
    private readonly password = ""
  ) {}

  async paths(): Promise<MediaMtxPath[]> {
    const response = await fetch(new URL("v3/paths/list", withTrailingSlash(this.baseUrl)), {
      headers: this.headers(),
      signal: AbortSignal.timeout(2000)
    });
    if (!response.ok) {
      throw new Error(`MediaMTX paths failed with ${response.status}.`);
    }
    const body = (await response.json()) as { items?: MediaMtxPath[] };
    return Array.isArray(body.items) ? body.items.filter((path) => typeof path.name === "string") : [];
  }

  async setInternalUsers(users: MediaMtxInternalUser[]): Promise<void> {
    const response = await fetch(new URL("v3/config/global/patch", withTrailingSlash(this.baseUrl)), {
      method: "PATCH",
      headers: this.headers({ "content-type": "application/json" }),
      body: JSON.stringify({ authInternalUsers: users }),
      signal: AbortSignal.timeout(2000)
    });
    if (!response.ok) {
      throw new Error(`MediaMTX auth update failed with ${response.status}.`);
    }
  }

  private headers(extra: Record<string, string> = {}): Record<string, string> {
    return this.username
      ? { ...extra, authorization: `Basic ${Buffer.from(`${this.username}:${this.password}`).toString("base64")}` }
      : extra;
  }
}

export function videoRoom(value: string | undefined): string | undefined {
  const trimmed = value?.trim().normalize("NFC") ?? "";
  const group = trimmed.startsWith("SB_") ? trimmed.slice(3) : trimmed;
  if (!group || group.length > 77 || /[\\/?#\u0000-\u001f\u007f]/u.test(group)) {
    return undefined;
  }
  return `SB_${group}`;
}

export function ingestVideoPath(value: string | undefined): string | undefined {
  const room = videoRoom(value);
  return room ? `ingest/${room}` : undefined;
}

export function groupFromPublicVideoPath(path: string): string | undefined {
  const room = videoRoom(path);
  return room === path ? room.slice(3) : undefined;
}

function withTrailingSlash(value: string): string {
  return value.endsWith("/") ? value : `${value}/`;
}
