import { randomUUID } from "node:crypto";
import pg from "pg";
import { hashPassword } from "./auth.js";

export type UserRecord = {
  id: string;
  username: string;
  passwordHash: string;
  role: "admin" | "user";
  createdAt: string;
};

export type RoomRecord = {
  id: string;
  name: string;
  createdBy: string;
  createdAt: string;
};

export type BanType = "udp-session" | "sonobus-udp" | "sonobus-connection";

export type BanRecord = {
  id: string;
  type: BanType;
  roomId?: string;
  userId?: string;
  group?: string;
  user?: string;
  address?: string;
  expiresAt: string | null;
  createdAt: string;
};

export type CreateBanInput = Omit<BanRecord, "id" | "createdAt">;

export type RemoveBanRequest = {
  id?: string;
  type?: BanType;
  roomId?: string;
  userId?: string;
  group?: string;
  user?: string;
  address?: string;
};

export type VideoControlRecord = {
  pairingId: string;
  group: string;
  user: string;
  pairingKeyCiphertext: string;
  groupPasswordCiphertext?: string;
  enabled: boolean;
  cameraDeviceId?: string;
  maxHeight: number;
  maxFps: number;
  maxBitrate: number;
  createdAt: string;
  updatedAt: string;
};

export type CreateVideoPairingInput = Pick<
  VideoControlRecord,
  "pairingId" | "group" | "user" | "pairingKeyCiphertext" | "groupPasswordCiphertext"
>;

export type SetVideoControlInput = {
  group: string;
  user: string;
  enabled: boolean;
  cameraDeviceId?: string | null;
  maxHeight?: number;
  maxFps?: number;
  maxBitrate?: number;
};

export interface Store {
  init(): Promise<void>;
  getUserByUsername(username: string): Promise<UserRecord | undefined>;
  getUserById(id: string): Promise<UserRecord | undefined>;
  createUser(username: string, password: string, role: "admin" | "user"): Promise<UserRecord>;
  updateUserCredentials(username: string, password: string, role: "admin" | "user"): Promise<UserRecord>;
  listRooms(): Promise<RoomRecord[]>;
  createRoom(name: string, createdBy: string): Promise<RoomRecord>;
  getRoom(id: string): Promise<RoomRecord | undefined>;
  listBans(): Promise<BanRecord[]>;
  createBan(input: CreateBanInput): Promise<BanRecord>;
  removeBans(request: RemoveBanRequest): Promise<BanRecord[]>;
  listVideoControls(): Promise<VideoControlRecord[]>;
  getVideoControl(group: string, user: string): Promise<VideoControlRecord | undefined>;
  createVideoPairing(input: CreateVideoPairingInput): Promise<VideoControlRecord>;
  setVideoControl(input: SetVideoControlInput): Promise<VideoControlRecord>;
  close(): Promise<void>;
}

export class MemoryStore implements Store {
  private users = new Map<string, UserRecord>();
  private rooms = new Map<string, RoomRecord>();
  private bans = new Map<string, BanRecord>();
  private videoControls = new Map<string, VideoControlRecord>();

  async init(): Promise<void> {}

  async getUserByUsername(username: string): Promise<UserRecord | undefined> {
    return [...this.users.values()].find((user) => user.username === username);
  }

  async getUserById(id: string): Promise<UserRecord | undefined> {
    return this.users.get(id);
  }

  async createUser(username: string, password: string, role: "admin" | "user"): Promise<UserRecord> {
    if (await this.getUserByUsername(username)) {
      throw new Error("Username already exists.");
    }
    const user: UserRecord = {
      id: randomUUID(),
      username,
      passwordHash: hashPassword(password),
      role,
      createdAt: new Date().toISOString()
    };
    this.users.set(user.id, user);
    return user;
  }

  async updateUserCredentials(username: string, password: string, role: "admin" | "user"): Promise<UserRecord> {
    const existing = await this.getUserByUsername(username);
    if (!existing) {
      throw new Error("User not found.");
    }
    const updated: UserRecord = {
      ...existing,
      passwordHash: hashPassword(password),
      role
    };
    this.users.set(updated.id, updated);
    return updated;
  }

  async listRooms(): Promise<RoomRecord[]> {
    return [...this.rooms.values()].sort((a, b) => a.createdAt.localeCompare(b.createdAt));
  }

  async createRoom(name: string, createdBy: string): Promise<RoomRecord> {
    const room: RoomRecord = {
      id: randomUUID(),
      name,
      createdBy,
      createdAt: new Date().toISOString()
    };
    this.rooms.set(room.id, room);
    return room;
  }

  async getRoom(id: string): Promise<RoomRecord | undefined> {
    return this.rooms.get(id);
  }

  async listBans(): Promise<BanRecord[]> {
    this.pruneExpiredBans();
    return [...this.bans.values()].sort((a, b) => a.createdAt.localeCompare(b.createdAt));
  }

  async createBan(input: CreateBanInput): Promise<BanRecord> {
    this.pruneExpiredBans();
    const ban: BanRecord = {
      ...input,
      id: randomUUID(),
      createdAt: new Date().toISOString()
    };
    this.bans.set(ban.id, ban);
    return ban;
  }

  async removeBans(request: RemoveBanRequest): Promise<BanRecord[]> {
    this.pruneExpiredBans();
    const removed: BanRecord[] = [];
    for (const [id, ban] of this.bans) {
      if (matchesBanRemoval(request, ban)) {
        removed.push(ban);
        this.bans.delete(id);
      }
    }
    return removed;
  }

  async listVideoControls(): Promise<VideoControlRecord[]> {
    return [...this.videoControls.values()].sort((a, b) => a.createdAt.localeCompare(b.createdAt));
  }

  async getVideoControl(group: string, user: string): Promise<VideoControlRecord | undefined> {
    return this.videoControls.get(videoControlKey(group, user));
  }

  async createVideoPairing(input: CreateVideoPairingInput): Promise<VideoControlRecord> {
    const existing = await this.getVideoControl(input.group, input.user);
    const now = new Date().toISOString();
    const control: VideoControlRecord = {
      ...input,
      enabled: false,
      maxHeight: 0,
      maxFps: 0,
      maxBitrate: 0,
      createdAt: existing?.createdAt ?? now,
      updatedAt: now
    };
    this.videoControls.set(videoControlKey(input.group, input.user), control);
    return control;
  }

  async setVideoControl(input: SetVideoControlInput): Promise<VideoControlRecord> {
    const key = videoControlKey(input.group, input.user);
    const existing = this.videoControls.get(key);
    if (!existing) {
      throw new Error("Video client is not paired.");
    }
    const now = new Date().toISOString();
    if (input.enabled) {
      for (const [otherKey, control] of this.videoControls) {
        if (otherKey !== key && control.group === input.group && control.enabled) {
          this.videoControls.set(otherKey, { ...control, enabled: false, updatedAt: now });
        }
      }
    }
    const updated: VideoControlRecord = {
      ...existing,
      enabled: input.enabled,
      cameraDeviceId: input.cameraDeviceId === undefined ? existing.cameraDeviceId : input.cameraDeviceId ?? undefined,
      maxHeight: input.maxHeight ?? existing.maxHeight,
      maxFps: input.maxFps ?? existing.maxFps,
      maxBitrate: input.maxBitrate ?? existing.maxBitrate,
      updatedAt: now
    };
    this.videoControls.set(key, updated);
    return updated;
  }

  async close(): Promise<void> {}

  private pruneExpiredBans(): void {
    const now = Date.now();
    for (const [id, ban] of this.bans) {
      if (ban.expiresAt !== null && new Date(ban.expiresAt).getTime() <= now) {
        this.bans.delete(id);
      }
    }
  }
}

export class PostgresStore implements Store {
  private pool: pg.Pool;

  constructor(connectionString: string) {
    this.pool = new pg.Pool({ connectionString });
  }

  async init(): Promise<void> {
    await this.pool.query(`
      create table if not exists users (
        id uuid primary key,
        username text not null unique,
        password_hash text not null,
        role text not null check (role in ('admin', 'user')),
        created_at timestamptz not null default now()
      );

      create table if not exists rooms (
        id uuid primary key,
        name text not null,
        created_by uuid not null references users(id),
        created_at timestamptz not null default now()
      );

      create table if not exists admin_bans (
        id uuid primary key,
        type text not null check (type in ('udp-session', 'sonobus-udp', 'sonobus-connection')),
        room_id text,
        user_id text,
        group_name text,
        user_name text,
        address text,
        expires_at timestamptz,
        created_at timestamptz not null default now()
      );

      create table if not exists video_controls (
        group_name text not null,
        user_name text not null,
        pairing_id uuid not null unique,
        pairing_key_ciphertext text not null,
        group_password_ciphertext text,
        enabled boolean not null default false,
        camera_device_id text,
        max_video_height integer not null default 0,
        max_video_fps double precision not null default 0,
        max_video_bitrate integer not null default 0,
        created_at timestamptz not null default now(),
        updated_at timestamptz not null default now(),
        primary key (group_name, user_name)
      );

      with ranked as (
        select ctid, row_number() over (partition by group_name order by updated_at desc, user_name) as position
        from video_controls where enabled = true
      )
      update video_controls set enabled = false where ctid in (select ctid from ranked where position > 1);

      create unique index if not exists video_controls_one_enabled_per_group
        on video_controls (group_name) where enabled = true;

      alter table video_controls add column if not exists group_password_ciphertext text;
      alter table video_controls add column if not exists max_video_height integer not null default 0;
      alter table video_controls add column if not exists max_video_fps double precision not null default 0;
      alter table video_controls add column if not exists max_video_bitrate integer not null default 0;
    `);
  }

  async getUserByUsername(username: string): Promise<UserRecord | undefined> {
    const result = await this.pool.query("select * from users where username = $1", [username]);
    return result.rows[0] ? mapUser(result.rows[0]) : undefined;
  }

  async getUserById(id: string): Promise<UserRecord | undefined> {
    const result = await this.pool.query("select * from users where id = $1", [id]);
    return result.rows[0] ? mapUser(result.rows[0]) : undefined;
  }

  async createUser(username: string, password: string, role: "admin" | "user"): Promise<UserRecord> {
    const id = randomUUID();
    const passwordHash = hashPassword(password);
    const result = await this.pool.query(
      "insert into users (id, username, password_hash, role) values ($1, $2, $3, $4) returning *",
      [id, username, passwordHash, role]
    );
    return mapUser(result.rows[0]);
  }

  async updateUserCredentials(username: string, password: string, role: "admin" | "user"): Promise<UserRecord> {
    const passwordHash = hashPassword(password);
    const result = await this.pool.query(
      "update users set password_hash = $1, role = $2 where username = $3 returning *",
      [passwordHash, role, username]
    );
    if (!result.rows[0]) {
      throw new Error("User not found.");
    }
    return mapUser(result.rows[0]);
  }

  async listRooms(): Promise<RoomRecord[]> {
    const result = await this.pool.query("select * from rooms order by created_at asc");
    return result.rows.map(mapRoom);
  }

  async createRoom(name: string, createdBy: string): Promise<RoomRecord> {
    const id = randomUUID();
    const result = await this.pool.query("insert into rooms (id, name, created_by) values ($1, $2, $3) returning *", [
      id,
      name,
      createdBy
    ]);
    return mapRoom(result.rows[0]);
  }

  async getRoom(id: string): Promise<RoomRecord | undefined> {
    const result = await this.pool.query("select * from rooms where id = $1", [id]);
    return result.rows[0] ? mapRoom(result.rows[0]) : undefined;
  }

  async listBans(): Promise<BanRecord[]> {
    await this.pruneExpiredBans();
    const result = await this.pool.query("select * from admin_bans order by created_at asc");
    return result.rows.map(mapBan);
  }

  async createBan(input: CreateBanInput): Promise<BanRecord> {
    await this.pruneExpiredBans();
    const id = randomUUID();
    const result = await this.pool.query(
      `insert into admin_bans (id, type, room_id, user_id, group_name, user_name, address, expires_at)
       values ($1, $2, $3, $4, $5, $6, $7, $8)
       returning *`,
      [id, input.type, input.roomId, input.userId, input.group, input.user, input.address, input.expiresAt]
    );
    return mapBan(result.rows[0]);
  }

  async removeBans(request: RemoveBanRequest): Promise<BanRecord[]> {
    await this.pruneExpiredBans();
    const bans = (await this.listBans()).filter((ban) => matchesBanRemoval(request, ban));
    if (!bans.length) {
      return [];
    }
    await this.pool.query("delete from admin_bans where id = any($1::uuid[])", [bans.map((ban) => ban.id)]);
    return bans;
  }

  async listVideoControls(): Promise<VideoControlRecord[]> {
    const result = await this.pool.query("select * from video_controls order by created_at asc");
    return result.rows.map(mapVideoControl);
  }

  async getVideoControl(group: string, user: string): Promise<VideoControlRecord | undefined> {
    const result = await this.pool.query("select * from video_controls where group_name = $1 and user_name = $2", [group, user]);
    return result.rows[0] ? mapVideoControl(result.rows[0]) : undefined;
  }

  async createVideoPairing(input: CreateVideoPairingInput): Promise<VideoControlRecord> {
    const result = await this.pool.query(
      `insert into video_controls (group_name, user_name, pairing_id, pairing_key_ciphertext, group_password_ciphertext, enabled, camera_device_id)
       values ($1, $2, $3, $4, $5, false, null)
       on conflict (group_name, user_name) do update set
         pairing_id = excluded.pairing_id,
         pairing_key_ciphertext = excluded.pairing_key_ciphertext,
         group_password_ciphertext = excluded.group_password_ciphertext,
         enabled = false,
         camera_device_id = null,
         max_video_height = 0,
         max_video_fps = 0,
         max_video_bitrate = 0,
         updated_at = now()
       returning *`,
      [input.group, input.user, input.pairingId, input.pairingKeyCiphertext, input.groupPasswordCiphertext]
    );
    return mapVideoControl(result.rows[0]);
  }

  async setVideoControl(input: SetVideoControlInput): Promise<VideoControlRecord> {
    const client = await this.pool.connect();
    try {
      await client.query("begin");
      await client.query("select pg_advisory_xact_lock(hashtextextended($1, 0))", [input.group]);
      if (input.enabled) {
        await client.query(
          "update video_controls set enabled = false, updated_at = now() where group_name = $1 and user_name <> $2 and enabled = true",
          [input.group, input.user]
        );
      }
      const result = await client.query(
        `update video_controls set
           enabled = $3,
           camera_device_id = case when $4::boolean then $5 else camera_device_id end,
           max_video_height = case when $6::boolean then $7 else max_video_height end,
           max_video_fps = case when $8::boolean then $9 else max_video_fps end,
           max_video_bitrate = case when $10::boolean then $11 else max_video_bitrate end,
           updated_at = now()
         where group_name = $1 and user_name = $2
         returning *`,
        [
          input.group, input.user, input.enabled,
          input.cameraDeviceId !== undefined, input.cameraDeviceId ?? null,
          input.maxHeight !== undefined, input.maxHeight ?? 0,
          input.maxFps !== undefined, input.maxFps ?? 0,
          input.maxBitrate !== undefined, input.maxBitrate ?? 0
        ]
      );
      if (!result.rows[0]) {
        throw new Error("Video client is not paired.");
      }
      await client.query("commit");
      return mapVideoControl(result.rows[0]);
    } catch (error) {
      await client.query("rollback");
      throw error;
    } finally {
      client.release();
    }
  }

  async close(): Promise<void> {
    await this.pool.end();
  }

  private async pruneExpiredBans(): Promise<void> {
    await this.pool.query("delete from admin_bans where expires_at is not null and expires_at <= now()");
  }
}

function mapUser(row: Record<string, unknown>): UserRecord {
  return {
    id: String(row.id),
    username: String(row.username),
    passwordHash: String(row.password_hash),
    role: row.role === "admin" ? "admin" : "user",
    createdAt: new Date(String(row.created_at)).toISOString()
  };
}

function mapRoom(row: Record<string, unknown>): RoomRecord {
  return {
    id: String(row.id),
    name: String(row.name),
    createdBy: String(row.created_by),
    createdAt: new Date(String(row.created_at)).toISOString()
  };
}

function mapBan(row: Record<string, unknown>): BanRecord {
  return {
    id: String(row.id),
    type: parseBanType(String(row.type)),
    roomId: optionalString(row.room_id),
    userId: optionalString(row.user_id),
    group: optionalString(row.group_name),
    user: optionalString(row.user_name),
    address: optionalString(row.address),
    expiresAt: row.expires_at === null || row.expires_at === undefined ? null : new Date(String(row.expires_at)).toISOString(),
    createdAt: new Date(String(row.created_at)).toISOString()
  };
}

function mapVideoControl(row: Record<string, unknown>): VideoControlRecord {
  return {
    pairingId: String(row.pairing_id),
    group: String(row.group_name),
    user: String(row.user_name),
    pairingKeyCiphertext: String(row.pairing_key_ciphertext),
    groupPasswordCiphertext: optionalString(row.group_password_ciphertext),
    enabled: Boolean(row.enabled),
    cameraDeviceId: optionalString(row.camera_device_id),
    maxHeight: Number(row.max_video_height ?? 0),
    maxFps: Number(row.max_video_fps ?? 0),
    maxBitrate: Number(row.max_video_bitrate ?? 0),
    createdAt: new Date(String(row.created_at)).toISOString(),
    updatedAt: new Date(String(row.updated_at)).toISOString()
  };
}

function videoControlKey(group: string, user: string): string {
  return `${group}\u0000${user}`;
}

function parseBanType(type: string): BanType {
  if (type === "udp-session" || type === "sonobus-udp" || type === "sonobus-connection") {
    return type;
  }
  throw new Error(`Invalid ban type: ${type}`);
}

function optionalString(value: unknown): string | undefined {
  return value === null || value === undefined ? undefined : String(value);
}

function matchesBanRemoval(request: RemoveBanRequest, ban: BanRecord): boolean {
  if (request.id) {
    return request.id === ban.id;
  }
  if (request.type && request.type !== ban.type) {
    return false;
  }
  if (request.roomId && request.roomId !== ban.roomId) {
    return false;
  }
  if (request.userId && request.userId !== ban.userId) {
    return false;
  }
  if (request.group && request.group !== ban.group) {
    return false;
  }
  if (request.user && request.user !== ban.user) {
    return false;
  }
  if (request.address && request.address !== ban.address) {
    return false;
  }
  return Boolean(request.type || request.roomId || request.userId || request.group || request.user || request.address);
}
