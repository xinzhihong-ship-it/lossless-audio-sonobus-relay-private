import { createCipheriv, createDecipheriv, createHash, createHmac, randomBytes, randomUUID, timingSafeEqual } from "node:crypto";
import type { ConnectionServerAdmin } from "./connectionServerAdmin.js";
import { ingestVideoPath, videoRoom, type MediaMtxAdmin, type MediaMtxInternalUser } from "./mediaMtx.js";
import type { Store, VideoControlRecord } from "./store.js";

const SESSION_TTL_MS = 15_000;
const ENROLLMENT_TTL_MS = 60_000;
const MAX_PENDING_ENROLLMENTS = 256;

export type VideoCamera = { id: string; name: string };

export type VideoClientStatus = {
  capturing: boolean;
  cameras: VideoCamera[];
  cameraDeviceId?: string;
  cameraName?: string;
  codec?: string;
  width?: number;
  height?: number;
  fps?: number;
  bitrate?: number;
  error?: string;
};

export type VideoControlView = Omit<VideoControlRecord, "pairingKeyCiphertext" | "groupPasswordCiphertext"> & VideoClientStatus & {
  online: boolean;
  clientId?: string;
};

export type VideoControlPollInput = {
  pairingId?: string;
  clientId?: string;
  timestamp?: number;
  sequence?: number;
  nonce?: string;
  status?: string;
  signature?: string;
};

export type VideoControlPollResponse = {
  payload: string;
  signature: string;
  pollAfterMs: number;
  serverTime: number;
};

export type VideoEnrollmentInput = {
  group?: string;
  user?: string;
  clientId?: string;
  timestamp?: number;
  nonce?: string;
  signature?: string;
};

export type VideoEnrollmentResponse = VideoControlPollResponse;

export type VideoEnrollmentView = {
  requestId: string;
  group: string;
  user: string;
  clientId: string;
  address: string;
  requestedAt: number;
  lastSeenAt: number;
};

type PendingEnrollment = VideoEnrollmentView & {
  key: Buffer;
  approvedPairingId?: string;
};

type ControlSession = {
  pairingId: string;
  group: string;
  user: string;
  clientId: string;
  key: Buffer;
  publishNonce: string;
  lastSequence: number;
  lastSeenAt: number;
  status: VideoClientStatus;
};

export class VideoControlService {
  private readonly encryptionKey: Buffer;
  private readonly sessions = new Map<string, ControlSession>();
  private readonly usedNonces = new Map<string, number>();
  private readonly pendingEnrollments = new Map<string, PendingEnrollment>();
  private readonly pruneTimer: NodeJS.Timeout;
  private stateChangeHandler?: () => void | Promise<void>;
  private pruning = false;
  private authSync: Promise<void> = Promise.resolve();

  constructor(
    private readonly store: Store,
    encryptionSecret: string,
    private readonly rtspPort = 19092,
    private readonly mediaMtx?: MediaMtxAdmin,
    private readonly muxerUsername = "media-muxer",
    private readonly muxerPassword = "",
    private readonly apiUsername = "media-api",
    private readonly apiPassword = ""
  ) {
    this.encryptionKey = createHash("sha256").update(`sonobus-video-control\0${encryptionSecret}`).digest();
    this.pruneTimer = setInterval(() => void this.pruneExpiredSessions(), 5000);
    this.pruneTimer.unref();
  }

  setStateChangeHandler(handler: () => void | Promise<void>): void {
    this.stateChangeHandler = handler;
  }


  async requestEnrollment(
    input: VideoEnrollmentInput,
    address: string,
    connectionServer: ConnectionServerAdmin
  ): Promise<VideoEnrollmentResponse> {
    const group = cleanString(input.group, 77);
    const user = cleanString(input.user, 80);
    const clientId = cleanString(input.clientId, 80);
    const nonce = cleanString(input.nonce, 120);
    const timestamp = Number(input.timestamp);
    const now = Date.now();
    if (!videoRoom(group) || !user || !clientId || !address || nonce.length < 16
        || !Number.isSafeInteger(timestamp) || Math.abs(now - timestamp) > 60_000) {
      throw new Error("Invalid video enrollment request.");
    }
    const replayKey = `enroll\0${clientId}\0${nonce}`;
    if (this.usedNonces.has(replayKey)) throw new Error("Video enrollment request was replayed.");

    const mapKey = `${group}\0${user}\0${clientId}`;
    let pending = this.pendingEnrollments.get(mapKey);
    let key = pending?.key;
    let expected = key ? sign(key, enrollmentSignatureInput(group, user, clientId, timestamp, nonce)) : "";
    if (!key || !safeEqual(input.signature, expected)) {
      const secret = await connectionServer.enrollmentSecret({ group, user, address });
      if (!/^[0-9a-f]{64}$/i.test(secret)) throw new Error("Invalid SonoBus enrollment capability.");
      key = deriveEnrollmentKey(Buffer.from(secret, "hex"), group, user, clientId);
      expected = sign(key, enrollmentSignatureInput(group, user, clientId, timestamp, nonce));
      if (!safeEqual(input.signature, expected)) throw new Error("Invalid video enrollment signature.");
      if (pending && !pending.key.equals(key)) pending = undefined;
    }

    if (!pending) {
      await this.pruneExpiredSessions();
      if (this.pendingEnrollments.size >= MAX_PENDING_ENROLLMENTS) throw new Error("Too many pending video enrollments.");
      pending = {
        requestId: randomUUID(),
        group,
        user,
        clientId,
        address,
        requestedAt: now,
        lastSeenAt: now,
        key
      };
      this.pendingEnrollments.set(mapKey, pending);
      await this.notifyStateChange();
    } else {
      pending.lastSeenAt = now;
      pending.address = address;
    }
    this.usedNonces.set(replayKey, now + 60_000);

    const payload = Buffer.from(JSON.stringify({
      type: "enrollment",
      state: pending.approvedPairingId ? "approved" : "pending",
      pairingId: pending.approvedPairingId
    })).toString("base64url");
    return {
      payload,
      signature: sign(key, `enrollment-response\n${nonce}\n${payload}`),
      pollAfterMs: 1000,
      serverTime: now
    };
  }

  async listPendingEnrollments(): Promise<VideoEnrollmentView[]> {
    await this.pruneExpiredSessions();
    return [...this.pendingEnrollments.values()]
      .filter((pending) => !pending.approvedPairingId)
      .map(({ key: _key, approvedPairingId: _approvedPairingId, ...view }) => view);
  }

  async approveEnrollment(requestId: string, groupPassword = ""): Promise<{ pairingId: string }> {
    const pending = [...this.pendingEnrollments.values()].find((candidate) => candidate.requestId === requestId);
    if (!pending || pending.lastSeenAt + ENROLLMENT_TTL_MS <= Date.now())
      throw new Error("Video enrollment request expired; reopen the client video panel.");
    const pairingId = await this.createPairingWithKey(pending.group, pending.user, pending.key, groupPassword);
    pending.approvedPairingId = pairingId;
    pending.lastSeenAt = Date.now();
    return { pairingId };
  }

  async list(): Promise<VideoControlView[]> {
    await this.pruneExpiredSessions();
    return (await this.store.listVideoControls()).map((control) => {
      const session = this.sessions.get(control.pairingId);
      return {
        pairingId: control.pairingId,
        group: control.group,
        user: control.user,
        enabled: control.enabled,
        cameraDeviceId: control.cameraDeviceId,
        createdAt: control.createdAt,
        updatedAt: control.updatedAt,
        online: Boolean(session),
        clientId: session?.clientId,
        capturing: session?.status.capturing ?? false,
        cameras: session?.status.cameras ?? [],
        cameraName: session?.status.cameraName,
        codec: session?.status.codec,
        width: session?.status.width,
        height: session?.status.height,
        fps: session?.status.fps,
        bitrate: session?.status.bitrate,
        error: session?.status.error
      };
    });
  }

  async activeGroups(): Promise<Array<{ group: string; groupPassword: string }>> {
    const now = Date.now();
    return (await this.store.listVideoControls())
      .filter((control) => control.enabled && (this.sessions.get(control.pairingId)?.lastSeenAt ?? 0) + SESSION_TTL_MS > now)
      .map((control) => ({
        group: control.group,
        groupPassword: control.groupPasswordCiphertext
          ? decryptKey(control.groupPasswordCiphertext, this.encryptionKey).toString("utf8")
          : ""
      }));
  }

  async setDesired(group: string, user: string, enabled: boolean, cameraDeviceId?: string | null): Promise<VideoControlRecord> {
    const updated = await this.store.setVideoControl({ group, user, enabled, cameraDeviceId });
    await this.syncMediaMtxAuth();
    await this.notifyStateChange();
    return updated;
  }

  async poll(input: VideoControlPollInput): Promise<VideoControlPollResponse> {
    const pairingId = cleanString(input.pairingId, 80);
    const clientId = cleanString(input.clientId, 80);
    const nonce = cleanString(input.nonce, 120);
    const status = cleanString(input.status, 32_768);
    const timestamp = Number(input.timestamp);
    const sequence = Number(input.sequence);
    const now = Date.now();
    if (!pairingId || !clientId || nonce.length < 16 || !status || !Number.isSafeInteger(timestamp) || Math.abs(now - timestamp) > 60_000 || !Number.isSafeInteger(sequence) || sequence < 1) {
      throw new Error("Invalid video control poll.");
    }
    const replayKey = `${pairingId}\0${nonce}`;
    if (this.usedNonces.has(replayKey)) throw new Error("Video control poll was replayed.");

    const control = (await this.store.listVideoControls()).find((candidate) => candidate.pairingId === pairingId);
    if (!control) throw new Error("Unknown video pairing.");
    const key = decryptKey(control.pairingKeyCiphertext, this.encryptionKey);
    const expected = sign(key, pollSignatureInput(pairingId, clientId, timestamp, sequence, nonce, status));
    if (!safeEqual(input.signature, expected)) throw new Error("Invalid video control signature.");

    const existing = this.sessions.get(pairingId);
    if (existing?.clientId === clientId && sequence <= existing.lastSequence) throw new Error("Video control sequence was replayed.");
    const isNewSession = !existing || existing.clientId !== clientId || existing.lastSeenAt + SESSION_TTL_MS <= now;
    const session: ControlSession = {
      pairingId,
      group: control.group,
      user: control.user,
      clientId,
      key,
      publishNonce: isNewSession ? randomBytes(24).toString("base64url") : existing.publishNonce,
      lastSequence: sequence,
      lastSeenAt: now,
      status: cleanStatus(JSON.parse(Buffer.from(status, "base64url").toString("utf8")) as Record<string, unknown>)
    };
    this.sessions.set(pairingId, session);
    this.usedNonces.set(replayKey, now + 60_000);
    for (const [mapKey, pending] of this.pendingEnrollments)
      if (pending.approvedPairingId === pairingId) this.pendingEnrollments.delete(mapKey);
    if (isNewSession) {
      await this.syncMediaMtxAuth();
      await this.notifyStateChange();
    }

    const freshControl = await this.store.getVideoControl(control.group, control.user);
    if (!freshControl) throw new Error("Video pairing disappeared.");
    const payload = Buffer.from(JSON.stringify(desiredPayload(freshControl, session, this.rtspPort))).toString("base64url");
    return {
      payload,
      signature: sign(key, `response\n${nonce}\n${payload}`),
      pollAfterMs: 1000,
      serverTime: now
    };
  }

  async syncMediaMtxAuth(): Promise<void> {
    if (!this.mediaMtx?.setInternalUsers) return;
    const next = this.authSync.catch(() => undefined).then(() => this.writeMediaMtxAuth());
    this.authSync = next;
    await next;
  }

  private async writeMediaMtxAuth(): Promise<void> {
    if (!this.mediaMtx?.setInternalUsers) return;
    const now = Date.now();
    const users: MediaMtxInternalUser[] = [];
    if (this.apiPassword) {
      users.push({
        user: this.apiUsername,
        pass: this.apiPassword,
        ips: [],
        permissions: [{ action: "api" }]
      });
    }
    users.push({
      user: "any",
      ips: [],
      permissions: [{ action: "read", path: "~^SB_[^/]+$" }]
    });
    if (this.muxerPassword) {
      users.push({
        user: this.muxerUsername,
        pass: this.muxerPassword,
        ips: [],
        permissions: [
          { action: "read", path: "~^ingest/SB_[^/]+$" },
          { action: "publish", path: "~^SB_[^/]+$" }
        ]
      });
    }
    for (const control of await this.store.listVideoControls()) {
      const session = this.sessions.get(control.pairingId);
      const path = ingestVideoPath(control.group);
      if (control.enabled && session && session.lastSeenAt + SESSION_TTL_MS > now && path) {
        users.push({
          user: `camera-${control.pairingId}`,
          pass: publishPassword(session),
          ips: [],
          permissions: [{ action: "publish", path }]
        });
      }
    }
    await this.mediaMtx.setInternalUsers(users);
  }

  async close(): Promise<void> {
    clearInterval(this.pruneTimer);
    this.sessions.clear();
    this.usedNonces.clear();
    this.pendingEnrollments.clear();
    await this.syncMediaMtxAuth();
  }

  private async pruneExpiredSessions(): Promise<void> {
    if (this.pruning) return;
    this.pruning = true;
    try {
      const now = Date.now();
      let changed = false;
      for (const [pairingId, session] of this.sessions) {
        if (session.lastSeenAt + SESSION_TTL_MS <= now) {
          this.sessions.delete(pairingId);
          changed = true;
        }
      }
      for (const [nonce, expiresAt] of this.usedNonces) {
        if (expiresAt <= now) this.usedNonces.delete(nonce);
      }
      for (const [mapKey, pending] of this.pendingEnrollments) {
        if (pending.lastSeenAt + ENROLLMENT_TTL_MS <= now) {
          this.pendingEnrollments.delete(mapKey);
          changed = true;
        }
      }
      if (changed) {
        await this.syncMediaMtxAuth();
        await this.notifyStateChange();
      }
    } finally {
      this.pruning = false;
    }
  }

  private async createPairingWithKey(group: string, user: string, key: Buffer, groupPassword: string): Promise<string> {
    if (key.length !== 32) throw new Error("Invalid video pairing key.");
    const existing = await this.store.getVideoControl(group, user);
    const pairingId = randomUUID();
    await this.store.createVideoPairing({
      pairingId,
      group,
      user,
      pairingKeyCiphertext: encryptKey(key, this.encryptionKey),
      groupPasswordCiphertext: groupPassword ? encryptKey(Buffer.from(groupPassword, "utf8"), this.encryptionKey) : undefined
    });
    if (existing) this.sessions.delete(existing.pairingId);
    await this.syncMediaMtxAuth();
    await this.notifyStateChange();
    return pairingId;
  }


  private async notifyStateChange(): Promise<void> {
    await this.stateChangeHandler?.();
  }
}

function desiredPayload(control: VideoControlRecord, session: ControlSession, rtspPort: number): Record<string, unknown> {
  const room = videoRoom(control.group);
  const ingestPath = ingestVideoPath(control.group);
  if (!room || !ingestPath) throw new Error("Invalid video group.");
  return {
    type: "desired",
    revision: control.updatedAt,
    enabled: control.enabled,
    cameraDeviceId: control.cameraDeviceId,
    room,
    ingestPath,
    rtspPort,
    publishUser: `camera-${control.pairingId}`,
    publishNonce: session.publishNonce,
    publishPasswordDerivation: "hmac-sha256/publish-pairing-publishNonce"
  };
}

function publishPassword(session: ControlSession): string {
  return sign(session.key, `publish\n${session.pairingId}\n${session.publishNonce}`);
}

function pollSignatureInput(pairingId: string, clientId: string, timestamp: number, sequence: number, nonce: string, status: string): string {
  return `poll\n${pairingId}\n${clientId}\n${timestamp}\n${sequence}\n${nonce}\n${status}`;
}

function enrollmentSignatureInput(group: string, user: string, clientId: string, timestamp: number, nonce: string): string {
  return `enroll\n${group}\n${user}\n${clientId}\n${timestamp}\n${nonce}`;
}

function deriveEnrollmentKey(secret: Buffer, group: string, user: string, clientId: string): Buffer {
  return createHash("sha256")
    .update("sonobus-video-enrollment-v1\0")
    .update(secret)
    .update(`\0${group}\0${user}\0${clientId}`)
    .digest();
}

function cleanStatus(value: Record<string, unknown>): VideoClientStatus {
  const cameras = Array.isArray(value.cameras)
    ? value.cameras.slice(0, 32).flatMap((camera) => {
        if (!camera || typeof camera !== "object") return [];
        const entry = camera as Record<string, unknown>;
        const id = cleanString(entry.id, 200);
        const name = cleanString(entry.name, 200);
        return id && name ? [{ id, name }] : [];
      })
    : [];
  return {
    capturing: value.capturing === true,
    cameras,
    cameraDeviceId: cleanString(value.cameraDeviceId, 200) || undefined,
    cameraName: cleanString(value.cameraName, 200) || undefined,
    codec: cleanString(value.codec, 40) || undefined,
    width: boundedNumber(value.width, 1, 16_384),
    height: boundedNumber(value.height, 1, 16_384),
    fps: boundedNumber(value.fps, 0, 240),
    bitrate: boundedNumber(value.bitrate, 0, 1_000_000_000),
    error: cleanString(value.error, 500) || undefined
  };
}

function boundedNumber(value: unknown, minimum: number, maximum: number): number | undefined {
  return typeof value === "number" && Number.isFinite(value) && value >= minimum && value <= maximum ? value : undefined;
}

function cleanString(value: unknown, maxLength: number): string {
  return typeof value === "string" ? value.trim().slice(0, maxLength) : "";
}

function sign(key: Buffer, value: string): string {
  return createHmac("sha256", key).update(value).digest("base64url");
}

function safeEqual(actual: unknown, expected: string): boolean {
  if (typeof actual !== "string") return false;
  const left = Buffer.from(actual);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

function encryptKey(key: Buffer, encryptionKey: Buffer): string {
  const iv = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", encryptionKey, iv);
  const encrypted = Buffer.concat([cipher.update(key), cipher.final()]);
  return ["v1", iv.toString("base64url"), encrypted.toString("base64url"), cipher.getAuthTag().toString("base64url")].join(".");
}

function decryptKey(value: string, encryptionKey: Buffer): Buffer {
  const [version, iv, encrypted, tag] = value.split(".");
  if (version !== "v1" || !iv || !encrypted || !tag) throw new Error("Invalid pairing key.");
  const decipher = createDecipheriv("aes-256-gcm", encryptionKey, Buffer.from(iv, "base64url"));
  decipher.setAuthTag(Buffer.from(tag, "base64url"));
  return Buffer.concat([decipher.update(Buffer.from(encrypted, "base64url")), decipher.final()]);
}
