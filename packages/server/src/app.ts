import http, { type IncomingMessage, type ServerResponse } from "node:http";
import { randomUUID } from "node:crypto";
import { URL } from "node:url";
import { WebSocketServer } from "ws";
import { encodeAudioFrame, type AudioFrameHeader } from "@lossless-audio/protocol";
import { signToken, verifyPassword, verifyToken, type TokenClaims } from "./auth.js";
import { HttpConnectionServerAdmin, type ConnectionServerAdmin, type ConnectionServerConnection } from "./connectionServerAdmin.js";
import { GroupMediaManager, type GroupMediaManagerConfig } from "./groupMediaManager.js";
import { RoomHub, type WebSocketConnection } from "./roomHub.js";
import { HttpMediaMtxAdmin, videoRoom, type MediaMtxAdmin } from "./mediaMtx.js";
import { MemoryStore, PostgresStore, type BanRecord, type BanType, type Store } from "./store.js";
import { UdpRelay, type UdpRelayConnection } from "./udpRelay.js";
import { VideoControlService } from "./videoControl.js";

export type ServerConfig = {
  jwtSecret: string;
  adminUsername: string;
  adminPassword: string;
  databaseUrl?: string;
  maxBytesPerSecondPerClient: number;
  udpRelayPort?: number;
  udpRawPeerTtlMs?: number;
  connectionServerAdminUrl?: string;
  webBridgeAdminUrl?: string;
  publicVideoHost?: string;
  publicHttpPort?: number;
  mediaMtxAdminUrl?: string;
  mediaMtxApiUsername?: string;
  mediaMtxApiPassword?: string;
  videoRtspPort?: number;
  mediaMuxerUsername?: string;
  mediaMuxerPassword?: string;
  groupMediaManagerConfig?: GroupMediaManagerConfig;
  connectionServer?: ConnectionServerAdmin;
  mediaMtxAdmin?: MediaMtxAdmin;
  store?: Store;
};

export type App = {
  server: http.Server;
  store: Store;
  close(): Promise<void>;
};

type BridgeGroupCache = {
  group?: string;
  expiresAt: number;
};

type AdminConnection = WebSocketConnection | UdpRelayConnection | ConnectionServerConnection;
type MergedSonoBusConnection = ConnectionServerConnection & {
  type: "sonobus-connection";
  hasRelay?: boolean;
  relayGroup?: string;
  relayUser?: string;
  relayAddress?: string;
  relayPort?: number;
  packetsReceived?: number;
  packetsForwarded?: number;
  bytesReceived?: number;
  bytesForwarded?: number;
  lastPacketType?: number;
  lastPacketBytes?: number;
  lastForwardCount?: number;
};

export async function createApp(config: ServerConfig): Promise<App> {
  const store = config.store ?? (config.databaseUrl ? new PostgresStore(config.databaseUrl) : new MemoryStore());
  await store.init();
  await ensureAdmin(store, config.adminUsername, config.adminPassword);
  const webBridgeAdminUrl = config.webBridgeAdminUrl;
  const bridgeGroupCache: BridgeGroupCache = { expiresAt: 0 };

  const hub = new RoomHub({
    maxBytesPerSecondPerClient: config.maxBytesPerSecondPerClient,
    audioFrameSink: webBridgeAdminUrl
      ? (frame, sender) => postWebBridgeAudioFrame(webBridgeAdminUrl, store, bridgeGroupCache, frame, sender)
      : undefined
  });
  const udpRelay = config.udpRelayPort === undefined ? undefined : new UdpRelay(config.udpRelayPort, config.udpRawPeerTtlMs);
  const connectionServer = config.connectionServer ?? (config.connectionServerAdminUrl ? new HttpConnectionServerAdmin(config.connectionServerAdminUrl) : undefined);
  const mediaMtx = config.mediaMtxAdmin ?? (config.mediaMtxAdminUrl
    ? new HttpMediaMtxAdmin(config.mediaMtxAdminUrl, config.mediaMtxApiUsername, config.mediaMtxApiPassword)
    : undefined);
  const videoControl = new VideoControlService(
    store,
    config.jwtSecret,
    config.videoRtspPort,
    mediaMtx,
    config.mediaMuxerUsername,
    config.mediaMuxerPassword,
    config.mediaMtxApiUsername,
    config.mediaMtxApiPassword
  );
  const groupMedia = mediaMtx && config.groupMediaManagerConfig
    ? new GroupMediaManager(config.groupMediaManagerConfig, mediaMtx)
    : undefined;
  videoControl.setStateChangeHandler(async () => groupMedia?.reconcile(await videoControl.activeGroups()));
  const bridgePoller = webBridgeAdminUrl ? startWebBridgeAudioPoller(webBridgeAdminUrl, store, hub) : undefined;
  await udpRelay?.start();
  await videoControl.syncMediaMtxAuth();
  groupMedia?.reconcile(await videoControl.activeGroups());
  await restorePersistentBans(store, udpRelay, connectionServer);
  const server = http.createServer((req, res) => {
    handleHttp(req, res, store, config, hub, udpRelay, connectionServer, videoControl, mediaMtx, groupMedia).catch((error) => {
      sendJson(res, 500, { error: error instanceof Error ? error.message : "Internal server error." });
    });
  });

  const wss = new WebSocketServer({ noServer: true });
  server.on("upgrade", async (req, socket, head) => {
    try {
      const url = new URL(req.url ?? "/", "http://localhost");

      const match = url.pathname.match(/^\/rooms\/([^/]+)\/stream$/);
      if (!match) {
        socket.destroy();
        return;
      }

      const token = url.searchParams.get("token") ?? "";
      const claims = verifyToken(token, config.jwtSecret);
      const room = await store.getRoom(match[1]);
      if (!room) {
        socket.destroy();
        return;
      }

      wss.handleUpgrade(req, socket, head, (ws) => {
        hub.join(room.id, claims.sub, claims.username, ws);
      });
    } catch {
      socket.destroy();
    }
  });

  return {
    server,
    store,
    async close() {
      groupMedia?.close();
      await videoControl.close();
      wss.close();
      bridgePoller?.stop();
      await new Promise<void>((resolve, reject) => server.close((error) => (error ? reject(error) : resolve())));
      await udpRelay?.stop();
      await store.close();
    }
  };
}

async function handleHttp(
  req: IncomingMessage,
  res: ServerResponse,
  store: Store,
  config: ServerConfig,
  hub: RoomHub,
  udpRelay: UdpRelay | undefined,
  connectionServer: ConnectionServerAdmin | undefined,
  videoControl: VideoControlService,
  mediaMtx: MediaMtxAdmin | undefined,
  groupMedia: GroupMediaManager | undefined
): Promise<void> {
  const url = new URL(req.url ?? "/", "http://localhost");

  if (req.method === "GET" && url.pathname === "/health") {
    sendJson(res, 200, { ok: true });
    return;
  }

  if (req.method === "GET" && (url.pathname === "/admin" || url.pathname === "/admin/")) {
    sendHtml(res, 200, adminPageHtml);
    return;
  }

  if (req.method === "GET" && (url.pathname === "/web" || url.pathname === "/web/" || url.pathname === "/join" || url.pathname === "/join/")) {
    sendHtml(res, 200, webJoinPageHtml);
    return;
  }

  if (req.method === "GET" && (url.pathname === "/video/view" || url.pathname === "/video/view/")) {
    const room = videoRoom(url.searchParams.get("room") ?? undefined);
    if (!room) {
      sendJson(res, 400, { error: "A valid SonoBus video room is required." });
      return;
    }
    res.writeHead(302, { location: `/${encodeURIComponent(room)}` });
    res.end();
    return;
  }

  if (req.method === "POST" && url.pathname === "/video/control/poll") {
    try {
      sendJson(res, 200, await videoControl.poll(await readJson(req)));
    } catch (error) {
      sendJson(res, 403, { error: error instanceof Error ? error.message : "Video control denied." });
    }
    return;
  }

  if (req.method === "POST" && url.pathname === "/auth/login") {
    const body = await readJson<{ username?: string; password?: string }>(req);
    if (!body.username || !body.password) {
      sendJson(res, 400, { error: "username and password are required." });
      return;
    }

    const user = await store.getUserByUsername(body.username);
    if (!user || !verifyPassword(body.password, user.passwordHash)) {
      sendJson(res, 401, { error: "Invalid credentials." });
      return;
    }

    sendJson(res, 200, {
      token: signToken({ sub: user.id, username: user.username, role: user.role }, config.jwtSecret),
      user: { id: user.id, username: user.username, role: user.role }
    });
    return;
  }

  if (req.method === "POST" && url.pathname === "/web/join") {
    const body = await readJson<{ roomName?: string; username?: string }>(req);
    const roomName = cleanWebField(body.roomName, 80);
    const username = cleanWebField(body.username, 48);
    if (!roomName || !username) {
      sendJson(res, 400, { error: "roomName and username are required." });
      return;
    }

    const adminUser = await store.getUserByUsername(config.adminUsername);
    if (!adminUser) {
      sendJson(res, 500, { error: "Admin user is not initialized." });
      return;
    }

    const room = await findOrCreateRoomByName(store, roomName, adminUser.id);
    const bridgeStatus = await getWebBridgeStatus(config.webBridgeAdminUrl);
    const userId = `web-${randomUUID()}`;
    const token = signToken({ sub: userId, username, role: "user" }, config.jwtSecret, 60 * 60 * 6);
    sendJson(res, 200, {
      token,
      user: { id: userId, username, role: "user" },
      room,
      members: hub.members(room.id),
      streamUrl: `/rooms/${room.id}/stream`,
      transport: "websocket-lpcm",
      bridge: {
        sonobusNativeInterop: webBridgeInteropEnabled(bridgeStatus),
        status: bridgeStatus,
        note: "Browser LPCM is bridged to the configured SonoBus/AoO group when the web-bridge service is reachable and joined."
      }
    });
    return;
  }


  const claims = authenticate(req, config);
  if (!claims) {
    sendJson(res, 401, { error: "Missing or invalid bearer token." });
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/users") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    const body = await readJson<{ username?: string; password?: string; role?: "admin" | "user" }>(req);
    if (!body.username || !body.password) {
      sendJson(res, 400, { error: "username and password are required." });
      return;
    }
    const user = await store.createUser(body.username, body.password, body.role ?? "user");
    sendJson(res, 201, { id: user.id, username: user.username, role: user.role });
    return;
  }

  if (req.method === "GET" && url.pathname === "/admin/video/controls") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    sendJson(res, 200, { controls: await videoControl.list(), paths: (await mediaMtx?.paths().catch(() => [])) ?? [] });
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/video/pair") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    const body = await readJson<{ group?: string; user?: string; groupPassword?: string }>(req);
    const group = cleanWebField(body.group, 77);
    const user = cleanWebField(body.user, 80);
    const groupPassword = typeof body.groupPassword === "string" ? body.groupPassword : "";
    if (Buffer.byteLength(groupPassword, "utf8") > 512) {
      sendJson(res, 400, { error: "Group password is too long." });
      return;
    }
    if (!videoRoom(group) || !user || isSystemBridgeUser(user)) {
      sendJson(res, 400, { error: "A valid group and user are required." });
      return;
    }
    if (connectionServer && !(await connectionServer.connections()).some((connection) => connection.group === group && connection.user === user)) {
      sendJson(res, 409, { error: "Pairing requires an active SonoBus connection." });
      return;
    }
    sendJson(res, 201, await videoControl.createPairing(group, user, groupPassword));
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/video/control") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    const body = await readJson<{ group?: string; user?: string; enabled?: boolean; cameraDeviceId?: string | null }>(req);
    const group = cleanWebField(body.group, 77);
    const user = cleanWebField(body.user, 80);
    if (!videoRoom(group) || !user || typeof body.enabled !== "boolean") {
      sendJson(res, 400, { error: "group, user, and enabled are required." });
      return;
    }
    const cameraDeviceId = body.cameraDeviceId === null ? null : cleanWebField(body.cameraDeviceId, 200) || undefined;
    const control = await videoControl.setDesired(group, user, body.enabled, cameraDeviceId);
    sendJson(res, 200, { control, controls: await videoControl.list() });
    return;
  }

  if (req.method === "GET" && url.pathname === "/admin/connections") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    sendJson(res, 200, {
      connections: mergeAdminConnections([
        ...hub.connections(),
        ...(udpRelay?.connections() ?? []),
        ...((await connectionServer?.connections()) ?? [])
      ]),
      videoControls: await videoControl.list(),
      videoPaths: (await mediaMtx?.paths().catch(() => [])) ?? [],
      videoUrls: config.publicVideoHost
        ? {
            browserBase: `http://${config.publicVideoHost}:${config.publicHttpPort ?? 19090}`,
            rtspBase: `rtsp://${config.publicVideoHost}:${config.videoRtspPort ?? 19092}`
          }
        : undefined,
      diagnostics: {
        udpRelay: udpRelay?.getDiagnostics(),
        groupMedia: groupMedia?.status()
      }
    });
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/connections/kick") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    const body = await readJson<{
      type?: "websocket" | "udp-session" | "sonobus-udp" | "sonobus-connection";
      roomId?: string;
      userId?: string;
      username?: string;
      sessionId?: string;
      group?: string;
      user?: string;
      address?: string;
      hasRelay?: boolean;
    }>(req);
    const udpKick =
      body.type === "websocket"
        ? undefined
        : body.type === "sonobus-connection"
          ? toUdpFromConnectionRequest(body)
          : {
              ...body,
              type: body.type
            };
    const websocketKicked = !body.type || body.type === "websocket" ? hub.kick(body) : 0;
    const udpResult = udpKick ? udpRelay?.kick(udpKick) : undefined;
    const connectionResult = body.type === "sonobus-connection" ? await connectionServer?.kick(toConnectionKick(body)) : undefined;
    sendJson(res, 200, {
      kicked: websocketKicked + (udpResult?.kicked ?? 0) + (connectionResult?.kicked ?? 0)
    });
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/bans") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    if (!udpRelay && !connectionServer) {
      sendJson(res, 503, { error: "UDP relay is disabled." });
      return;
    }
    const body = await readJson<{
      type?: "udp-session" | "sonobus-udp" | "sonobus-connection";
      roomId?: string;
      userId?: string;
      group?: string;
      user?: string;
      address?: string;
      hasRelay?: boolean;
      ttlSeconds?: number;
    }>(req);
    if (body.type === "sonobus-connection") {
      if (!connectionServer) {
        sendJson(res, 503, { error: "SonoBus connection server admin is disabled." });
        return;
      }
      const result = await connectionServer.ban(toConnectionBan(body));
      const udpResult = udpRelay?.ban(toUdpFromConnectionRequest(body));
      const ban = await store.createBan(toStoredBan(body, result.expiresAt));
      sendJson(res, 200, { banned: result.banned + (udpResult?.banned ?? 0), expiresAt: ban.expiresAt });
      return;
    }
    if (!udpRelay) {
      sendJson(res, 503, { error: "UDP relay is disabled." });
      return;
    }
    const result = udpRelay.ban(toUdpBan(body));
    const ban = await store.createBan(toStoredBan(body, result.expiresAt));
    sendJson(res, 200, { banned: result.banned, expiresAt: ban.expiresAt });
    return;
  }

  if (req.method === "GET" && url.pathname === "/admin/bans") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    if (!udpRelay && !connectionServer) {
      sendJson(res, 503, { error: "UDP relay is disabled." });
      return;
    }
    sendJson(res, 200, { bans: await store.listBans() });
    return;
  }

  if (req.method === "POST" && url.pathname === "/admin/bans/remove") {
    if (claims.role !== "admin") {
      sendJson(res, 403, { error: "Admin role required." });
      return;
    }
    if (!udpRelay && !connectionServer) {
      sendJson(res, 503, { error: "UDP relay is disabled." });
      return;
    }
    const body = await readJson<{
      id?: string;
      type?: "udp-session" | "sonobus-udp" | "sonobus-connection";
      roomId?: string;
      userId?: string;
      group?: string;
      user?: string;
      address?: string;
    }>(req);
    const removed = await store.removeBans(body);
    let removedFromServices = 0;
    for (const ban of removed) {
      if (ban.type === "sonobus-connection") {
        removedFromServices += (await connectionServer?.unban(toConnectionUnban(ban)))?.removed ?? 0;
        removedFromServices += udpRelay?.unban(toUdpUnban({ ...ban, type: "sonobus-udp", address: undefined }, false)).removed ?? 0;
      } else {
        removedFromServices += udpRelay?.unban(toUdpUnban(ban, false)).removed ?? 0;
      }
    }
    sendJson(res, 200, { removed: removed.length || removedFromServices });
    return;
  }

  if (req.method === "GET" && url.pathname === "/rooms") {
    sendJson(res, 200, { rooms: await store.listRooms() });
    return;
  }

  if (req.method === "POST" && url.pathname === "/rooms") {
    const body = await readJson<{ name?: string }>(req);
    if (!body.name) {
      sendJson(res, 400, { error: "room name is required." });
      return;
    }
    sendJson(res, 201, { room: await store.createRoom(body.name, claims.sub) });
    return;
  }

  const joinMatch = url.pathname.match(/^\/rooms\/([^/]+)\/join$/);
  if (req.method === "POST" && joinMatch) {
    const room = await store.getRoom(joinMatch[1]);
    if (!room) {
      sendJson(res, 404, { error: "Room not found." });
      return;
    }
    sendJson(res, 200, {
      room,
      members: hub.members(room.id),
      streamUrl: `/rooms/${room.id}/stream`
    });
    return;
  }

  const relayMatch = url.pathname.match(/^\/rooms\/([^/]+)\/relay-session$/);
  if (req.method === "POST" && relayMatch) {
    if (!udpRelay) {
      sendJson(res, 503, { error: "UDP relay is disabled." });
      return;
    }
    const room = await store.getRoom(relayMatch[1]);
    if (!room) {
      sendJson(res, 404, { error: "Room not found." });
      return;
    }
    const session = udpRelay.createSession(room.id, claims.sub, claims.username);
    sendJson(res, 201, {
      sessionId: session.sessionId,
      roomId: room.id,
      userId: claims.sub,
      udpPort: udpRelay.publicPort
    });
    return;
  }

  sendJson(res, 404, { error: "Not found." });
}

async function ensureAdmin(store: Store, username: string, password: string): Promise<void> {
  const existing = await store.getUserByUsername(username);
  if (!existing) {
    await store.createUser(username, password, "admin");
    return;
  }

  if (existing.role !== "admin" || !verifyPassword(password, existing.passwordHash)) {
    await store.updateUserCredentials(username, password, "admin");
  }
}

async function findOrCreateRoomByName(store: Store, name: string, createdBy: string) {
  const existing = (await store.listRooms()).find((room) => room.name === name);
  return existing ?? store.createRoom(name, createdBy);
}

function cleanWebField(value: string | undefined, maxLength: number): string {
  return (value ?? "").trim().replace(/\s+/g, " ").slice(0, maxLength);
}

function isSystemBridgeUser(user: string): boolean {
  return user === "web-bridge" || user.startsWith("media-mix-");
}

async function getWebBridgeStatus(adminUrl: string | undefined): Promise<unknown> {
  if (!adminUrl) {
    return { configured: false };
  }
  try {
    const response = await fetch(`${adminUrl.replace(/\/$/, "")}/status`);
    if (!response.ok) {
      return { configured: true, reachable: false, status: response.status };
    }
    return { configured: true, reachable: true, ...(await response.json()) as Record<string, unknown> };
  } catch (error) {
    return {
      configured: true,
      reachable: false,
      error: error instanceof Error ? error.message : "unknown bridge status error"
    };
  }
}

function webBridgeInteropEnabled(status: unknown): boolean {
  return Boolean(
    status &&
    typeof status === "object" &&
    "configured" in status &&
    "reachable" in status &&
    "connected" in status &&
    "joined" in status &&
    (status as { configured?: unknown }).configured === true &&
    (status as { reachable?: unknown }).reachable === true &&
    (status as { connected?: unknown }).connected === true &&
    (status as { joined?: unknown }).joined === true
  );
}

async function postWebBridgeAudioFrame(
  adminUrl: string,
  store: Store,
  bridgeGroupCache: BridgeGroupCache,
  frame: { header: { sampleRate: number; bitDepth: number; channels: number; sequence: number; timestamp: number; userId: string; streamId: string }; payload: Buffer },
  sender: WebSocketConnection
): Promise<void> {
  try {
    const [room, bridgeGroup] = await Promise.all([
      store.getRoom(sender.roomId),
      getCachedWebBridgeGroup(adminUrl, bridgeGroupCache)
    ]);
    if (!room || bridgeGroup !== room.name) {
      return;
    }
    await fetch(`${adminUrl.replace(/\/$/, "")}/audio/pcm`, {
      method: "POST",
      headers: {
        "content-type": "application/octet-stream",
        "x-room-id": sender.roomId,
        "x-user-id": sender.userId,
        "x-username": sender.username,
        "x-stream-id": frame.header.streamId,
        "x-sample-rate": String(frame.header.sampleRate),
        "x-bit-depth": String(frame.header.bitDepth),
        "x-channels": String(frame.header.channels),
        "x-sequence": String(frame.header.sequence),
        "x-timestamp": String(frame.header.timestamp)
      },
      body: frame.payload as unknown as BodyInit
    });
  } catch {
    // Browser rooms must keep working even when the optional native bridge is down.
  }
}

async function getCachedWebBridgeGroup(adminUrl: string, cache: BridgeGroupCache): Promise<string | undefined> {
  const now = Date.now();
  if (now < cache.expiresAt) {
    return cache.group;
  }
  const status = await getWebBridgeStatus(adminUrl) as WebBridgeStatus;
  cache.group = status.connected === true && status.joined === true && typeof status.group === "string" ? status.group : undefined;
  cache.expiresAt = now + 500;
  return cache.group;
}

type BridgeAudioPollFrame = {
  group?: string;
  userId?: string;
  username?: string;
  streamId?: string;
  sampleRate?: number;
  bitDepth?: 16 | 24 | 32;
  channels?: 1 | 2;
  sequence?: number;
  timestamp?: number;
  payload?: string;
};

type BridgePeerStatus = {
  group?: string;
  user?: string;
  connected?: boolean;
  sourceInvited?: boolean;
  nativeFramesOut?: number;
  sinkPackets?: number;
};

type WebBridgeStatus = {
  configured?: boolean;
  reachable?: boolean;
  connected?: boolean;
  joined?: boolean;
  group?: string;
  peers?: BridgePeerStatus[];
};

function startWebBridgeAudioPoller(adminUrl: string, store: Store, hub: RoomHub): { stop(): void } {
  let stopped = false;
  const baseUrl = adminUrl.replace(/\/$/, "");
  let lastStatusPoll = 0;
  const poll = async () => {
    if (stopped) {
      return;
    }
    try {
      const response = await fetch(`${baseUrl}/audio/pcm`);
      if (response.ok) {
        const body = (await response.json()) as { frames?: BridgeAudioPollFrame[] };
        for (const frame of body.frames ?? []) {
          await broadcastBridgeAudioFrame(store, hub, frame);
        }
      }
      const now = Date.now();
      if (now - lastStatusPoll >= 500) {
        lastStatusPoll = now;
        const statusResponse = await fetch(`${baseUrl}/status`);
        if (statusResponse.ok) {
          await publishBridgePeerStatus(store, hub, (await statusResponse.json()) as { group?: string; peers?: BridgePeerStatus[] });
        }
      }
    } catch {
      // Optional bridge polling should not affect the HTTP/WebSocket service.
    } finally {
      if (!stopped) {
        setTimeout(poll, 5);
      }
    }
  };
  setTimeout(poll, 5);
  return {
    stop() {
      stopped = true;
    }
  };
}

async function publishBridgePeerStatus(store: Store, hub: RoomHub, status: { group?: string; peers?: BridgePeerStatus[] }): Promise<void> {
  const group = status.group;
  if (!group) {
    return;
  }
  const room = (await store.listRooms()).find((candidate) => candidate.name === group);
  if (!room) {
    return;
  }
  const members = (status.peers ?? [])
    .filter((peer) => peer.group === group && peer.user && peer.connected !== false)
    .map((peer) => ({
      userId: `sonobus-${peer.user ?? "unknown"}`,
      username: peer.user ?? "SonoBus",
      streamId: `sonobus-${peer.user ?? "unknown"}-native`,
      format: {
        sampleRate: 48000,
        bitDepth: 24 as const,
        channels: 2 as const
      }
    }));
  hub.publishBridgeMembers(room.id, members);
}

async function broadcastBridgeAudioFrame(store: Store, hub: RoomHub, frame: BridgeAudioPollFrame): Promise<void> {
  if (!frame.group || !frame.userId || !frame.streamId || !frame.payload) {
    return;
  }
  const room = (await store.listRooms()).find((candidate) => candidate.name === frame.group);
  if (!room) {
    return;
  }
  const header: AudioFrameHeader = {
    streamId: frame.streamId,
    userId: frame.userId,
    sampleRate: frame.sampleRate ?? 48000,
    bitDepth: frame.bitDepth ?? 24,
    channels: frame.channels ?? 2,
    sequence: frame.sequence ?? 0,
    timestamp: frame.timestamp ?? Date.now()
  };
  const payload = Buffer.from(frame.payload, "base64");
  const raw = encodeAudioFrame(header, payload);
  hub.broadcastBridgeAudioFrame(
    room.id,
    {
      userId: frame.userId,
      username: frame.username ?? frame.userId,
      streamId: frame.streamId,
      format: {
        sampleRate: header.sampleRate,
        bitDepth: header.bitDepth,
        channels: header.channels
      }
    },
    raw
  );
}

function authenticate(req: IncomingMessage, config: ServerConfig): TokenClaims | undefined {
  const auth = req.headers.authorization ?? "";
  const match = auth.match(/^Bearer (.+)$/);
  if (!match) {
    return undefined;
  }
  try {
    return verifyToken(match[1], config.jwtSecret);
  } catch {
    return undefined;
  }
}

function toConnectionKick(body: { group?: string; user?: string; address?: string }) {
  return compact({ type: "sonobus-connection" as const, group: body.group, user: body.user, address: body.address });
}

function toUdpFromConnectionRequest(body: { group?: string; user?: string; hasRelay?: boolean }) {
  return compact({ type: "sonobus-udp" as const, group: body.group, user: body.user });
}

function mergeAdminConnections(connections: AdminConnection[]): AdminConnection[] {
  const merged: AdminConnection[] = [];
  const sonobusConnections = connections.filter((connection): connection is MergedSonoBusConnection => connection.type === "sonobus-connection");

  for (const connection of connections) {
    if (connection.type !== "sonobus-udp") {
      merged.push(connection);
      continue;
    }

    const existing = sonobusConnections.find((candidate) => sameSonoBusPeer(candidate, connection));
    if (!existing) {
      merged.push(connection);
      continue;
    }

    if (!existing.lastSeenAt || new Date(connection.lastSeenAt).getTime() > new Date(existing.lastSeenAt).getTime()) {
      existing.lastSeenAt = connection.lastSeenAt;
    }
    existing.hasRelay = true;
    existing.relayGroup = connection.group;
    existing.relayUser = connection.user;
    existing.relayAddress = connection.address;
    existing.relayPort = connection.port;
    existing.packetsReceived = connection.packetsReceived;
    existing.packetsForwarded = connection.packetsForwarded;
    existing.bytesReceived = connection.bytesReceived;
    existing.bytesForwarded = connection.bytesForwarded;
    existing.lastPacketType = connection.lastPacketType;
    existing.lastPacketBytes = connection.lastPacketBytes;
    existing.lastForwardCount = connection.lastForwardCount;
    existing.address ??= connection.address;
    existing.port ??= connection.port;
  }

  return merged;
}

function sameSonoBusPeer(connection: ConnectionServerConnection, relay: Extract<UdpRelayConnection, { type: "sonobus-udp" }>): boolean {
  return (connection.group ?? "") === relay.group && connection.user === relay.user;
}

function toConnectionBan(body: { group?: string; user?: string; address?: string; ttlSeconds?: number }) {
  return compact({ ...toConnectionKick(body), ttlSeconds: body.ttlSeconds });
}

function toConnectionUnban(body: { id?: string; group?: string; user?: string; address?: string }, preferId = false) {
  if (preferId && body.id) {
    return { id: body.id };
  }
  return compact(toConnectionKick(body));
}

function toUdpBan(body: { type?: "udp-session" | "sonobus-udp" | "sonobus-connection"; roomId?: string; userId?: string; group?: string; user?: string; address?: string; ttlSeconds?: number }) {
  return {
    type: body.type === "udp-session" ? "udp-session" as const : "sonobus-udp" as const,
    roomId: body.roomId,
    userId: body.userId,
    group: body.group,
    user: body.user,
    address: body.address,
    ttlSeconds: body.ttlSeconds
  };
}

function toUdpUnban(
  body: { id?: string; type?: "udp-session" | "sonobus-udp" | "sonobus-connection"; roomId?: string; userId?: string; group?: string; user?: string; address?: string },
  preferId = true
) {
  return {
    id: preferId ? body.id : undefined,
    type: body.type === "udp-session" ? "udp-session" as const : "sonobus-udp" as const,
    roomId: body.roomId,
    userId: body.userId,
    group: body.group,
    user: body.user,
    address: body.address
  };
}

async function restorePersistentBans(store: Store, udpRelay?: UdpRelay, connectionServer?: ConnectionServerAdmin): Promise<void> {
  const bans = await store.listBans();
  for (const ban of bans) {
    if (ban.type === "sonobus-connection") {
      await connectionServer?.ban(toConnectionBan(toRestoredBanRequest(ban)));
      udpRelay?.restoreBan(toUdpBanRecord(ban));
    } else {
      udpRelay?.restoreBan(toUdpBanRecord(ban));
    }
  }
}

function toStoredBan(
  body: { type?: "udp-session" | "sonobus-udp" | "sonobus-connection"; roomId?: string; userId?: string; group?: string; user?: string; address?: string; ttlSeconds?: number },
  serviceExpiresAt: string | null
) {
  return {
    type: storedBanType(body.type),
    roomId: body.roomId,
    userId: body.userId,
    group: body.group,
    user: body.user,
    address: body.address,
    expiresAt: body.ttlSeconds !== undefined ? expiresAtForTtl(body.ttlSeconds) : serviceExpiresAt
  };
}

function storedBanType(type: "udp-session" | "sonobus-udp" | "sonobus-connection" | undefined): BanType {
  if (type === "udp-session" || type === "sonobus-connection") {
    return type;
  }
  return "sonobus-udp";
}

function expiresAtForTtl(ttlSeconds: number): string | null {
  if (ttlSeconds <= 0) {
    return null;
  }
  const clamped = Math.max(1, Math.min(Number(ttlSeconds), 30 * 24 * 60 * 60));
  return new Date(Date.now() + clamped * 1000).toISOString();
}

function ttlSecondsForBan(ban: BanRecord): number {
  if (ban.expiresAt === null) {
    return 0;
  }
  return Math.max(1, Math.ceil((new Date(ban.expiresAt).getTime() - Date.now()) / 1000));
}

function toRestoredBanRequest(ban: BanRecord) {
  return {
    group: ban.group,
    user: ban.user,
    address: ban.address,
    ttlSeconds: ttlSecondsForBan(ban)
  };
}

function toUdpBanRecord(ban: BanRecord) {
  return {
    id: ban.id,
    type: ban.type === "udp-session" ? "udp-session" as const : "sonobus-udp" as const,
    roomId: ban.roomId,
    userId: ban.userId,
    group: ban.group,
    user: ban.user,
    address: ban.address,
    expiresAt: ban.expiresAt
  };
}

function compact<T extends Record<string, unknown>>(value: T): T {
  return Object.fromEntries(Object.entries(value).filter(([, entry]) => entry !== undefined)) as T;
}

async function readJson<T>(req: IncomingMessage): Promise<T> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  if (chunks.length === 0) {
    return {} as T;
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8")) as T;
}

function sendJson(res: ServerResponse, status: number, body: unknown): void {
  const payload = Buffer.from(JSON.stringify(body), "utf8");
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": payload.byteLength,
    "access-control-allow-origin": "*"
  });
  res.end(payload);
}

function sendHtml(res: ServerResponse, status: number, html: string): void {
  const payload = Buffer.from(html, "utf8");
  res.writeHead(status, {
    "content-type": "text/html; charset=utf-8",
    "content-length": payload.byteLength,
    "cache-control": "no-store"
  });
  res.end(payload);
}

const webJoinPageHtml = String.raw`<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Web 加入音频房间</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif;
      background: #101418;
      color: #eef3f7;
    }
    * {
      box-sizing: border-box;
    }
    body {
      margin: 0;
      background: #101418;
    }
    main {
      width: min(1040px, 100%);
      margin: 0 auto;
      padding: 22px;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 16px;
      margin-bottom: 18px;
    }
    h1 {
      margin: 0;
      font-size: 24px;
      letter-spacing: 0;
    }
    p {
      margin: 6px 0 0;
      color: #aab7c3;
      line-height: 1.5;
    }
    .badge {
      min-height: 28px;
      padding: 5px 10px;
      border: 1px solid #365043;
      border-radius: 999px;
      color: #bfe8ce;
      background: #142018;
      font-size: 12px;
      font-weight: 700;
      white-space: nowrap;
    }
    .grid {
      display: grid;
      grid-template-columns: 360px 1fr;
      gap: 14px;
    }
    section {
      border: 1px solid #2a3540;
      border-radius: 8px;
      background: #171f27;
      padding: 16px;
    }
    h2 {
      margin: 0 0 14px;
      font-size: 16px;
      letter-spacing: 0;
    }
    label {
      display: grid;
      gap: 6px;
      margin-bottom: 12px;
      color: #aab7c3;
      font-size: 13px;
    }
    input,
    select,
    button {
      min-height: 38px;
      border: 1px solid #3a4a59;
      border-radius: 6px;
      background: #0f1419;
      color: #eef3f7;
      padding: 0 10px;
      font: inherit;
    }
    button {
      cursor: pointer;
      border-color: #2b7b68;
      background: #1b6958;
      font-weight: 700;
    }
    button.secondary {
      border-color: #425467;
      background: #263341;
    }
    button:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .row > button,
    .row > select {
      flex: 1 1 150px;
    }
    .meter {
      width: 100%;
      height: 10px;
      overflow: hidden;
      border-radius: 999px;
      background: #0f1419;
      border: 1px solid #2a3540;
    }
    .meter > div {
      width: 0%;
      height: 100%;
      background: #d6a23c;
      transition: width 80ms linear;
    }
    .status {
      min-height: 22px;
      color: #bfe8ce;
      font-size: 13px;
      line-height: 1.5;
    }
    .error {
      color: #ffb0a8;
    }
    .latency {
      min-height: 22px;
      margin-top: 10px;
      color: #aab7c3;
      font-size: 13px;
      line-height: 1.45;
    }
    .permission-panel {
      display: grid;
      gap: 10px;
      grid-column: 1 / -1;
    }
    .permission-panel .status {
      margin: 0;
    }
    .permission-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
    }
    .permission-item {
      min-height: 120px;
      padding: 10px;
      border: 1px solid #2a3540;
      border-radius: 8px;
      background: #111820;
    }
    .permission-item strong {
      display: block;
      margin-bottom: 6px;
      font-size: 13px;
    }
    .permission-item small {
      color: #aab7c3;
      line-height: 1.5;
    }
    .members {
      display: grid;
      gap: 8px;
    }
    .member {
      display: grid;
      grid-template-columns: minmax(120px, 1fr) auto auto;
      align-items: center;
      gap: 10px;
      min-height: 46px;
      padding: 10px;
      border: 1px solid #2a3540;
      border-radius: 8px;
      background: #111820;
    }
    .member small {
      display: block;
      margin-top: 3px;
      color: #aab7c3;
    }
    .member button {
      min-height: 32px;
      padding: 0 10px;
      border-color: #425467;
      background: #263341;
      font-size: 12px;
    }
    .member button.active {
      border-color: #8d5b2d;
      background: #6b421d;
      color: #ffd7a5;
    }
    .empty {
      color: #aab7c3;
    }
    @media (max-width: 860px) {
      header,
      .grid {
        display: grid;
        grid-template-columns: 1fr;
      }
      main {
        padding: 14px;
      }
      .permission-grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>Web 加入音频房间</h1>
        <p>浏览器直接采集麦克风，通过服务器 WebSocket 转发无压缩 PCM；bridge 在线时会转接到同名 SonoBus group。</p>
      </div>
      <div class="badge" id="transportBadge">WebSocket LPCM</div>
    </header>

    <div class="grid">
      <section>
        <h2>加入</h2>
        <label>房间名
          <input id="roomName" maxlength="80" value="studio" autocomplete="off">
        </label>
        <label>显示名
          <input id="username" maxlength="48" value="web-user" autocomplete="off">
        </label>
        <label>输入设备
          <select id="inputDevice"></select>
        </label>
        <label>音质
          <select id="qualitySelect">
            <option value="48000-24-2" selected>48kHz / 24bit / 双声道</option>
            <option value="48000-16-2">48kHz / 16bit / 双声道</option>
            <option value="48000-24-1">48kHz / 24bit / 单声道</option>
            <option value="48000-16-1">48kHz / 16bit / 单声道</option>
          </select>
        </label>
        <label>接收缓冲 ms
          <input id="playbackBufferMs" type="number" min="5" max="200" step="5" value="25">
        </label>
        <label>发送延迟
          <select id="sendLatencySelect">
            <option value="256">极低 256 samples</option>
            <option value="512" selected>标准 512 samples</option>
            <option value="1024">稳定 1024 samples</option>
            <option value="2048">高稳定 2048 samples</option>
            <option value="4096">最稳 4096 samples</option>
          </select>
        </label>
        <div class="row">
          <button id="refreshDevices" class="secondary" type="button">刷新设备</button>
          <button id="joinButton" type="button">加入并开麦</button>
        </div>
        <p class="status" id="status">未连接</p>
      </section>

      <section>
        <h2>发送</h2>
        <div class="meter" aria-label="input level"><div id="inputMeter"></div></div>
        <p id="qualitySummary">采样率 48000Hz，24bit，双声道，发送 512 samples。浏览器需要允许麦克风权限。</p>
        <div class="row">
          <button id="startButton" type="button" disabled>开始发送</button>
          <button id="stopButton" class="secondary" type="button" disabled>停止发送</button>
        </div>
        <div class="latency" id="latencyStats">未收到远端音频。</div>
      </section>

      <section class="permission-panel">
        <h2>麦克风权限</h2>
        <p class="status" id="permissionStatus">正在检查当前浏览器权限环境。</p>
        <div class="row">
          <button id="testMicButton" class="secondary" type="button">测试麦克风权限</button>
          <button id="copyWebUrlButton" class="secondary" type="button">复制当前入口</button>
          <button id="copyHttpsUrlButton" class="secondary" type="button">复制 HTTPS 入口</button>
          <button id="copyChromeFlagButton" class="secondary" type="button">复制 Chrome/Edge 临时允许命令</button>
        </div>
        <div class="permission-grid">
          <div class="permission-item">
            <strong>Chrome / Edge</strong>
            <small>推荐 HTTPS。没有域名时，地址栏左侧站点设置里允许麦克风；若 HTTP 公网 IP 被拦，可用 Chrome/Edge 临时允许命令启动浏览器。</small>
          </div>
          <div class="permission-item">
            <strong>Firefox</strong>
            <small>推荐 HTTPS。点击地址栏左侧权限图标允许麦克风；HTTP 公网 IP 可能被浏览器策略拦截。</small>
          </div>
          <div class="permission-item">
            <strong>Safari / iOS</strong>
            <small>基本需要 HTTPS。进入 Safari 网站设置允许麦克风；公网 IP 的 HTTP 页面通常不能稳定使用采集权限。</small>
          </div>
          <div class="permission-item">
            <strong>没有域名</strong>
            <small>最稳做法是买域名并绑定 HTTPS。网页不能一键替用户开启权限，只能引导复制地址或命令后手动允许。</small>
          </div>
        </div>
      </section>

      <section style="grid-column: 1 / -1;">
        <h2>房间成员</h2>
        <div class="members" id="members"><div class="empty">加入后显示在线成员、开麦状态和远端音频格式。</div></div>
      </section>
    </div>
  </main>

  <script>
    const defaultFormat = { sampleRate: 48000, bitDepth: 24, channels: 2 };
    const playbackLeadSeconds = 0.025;
    const playbackMinLeadSeconds = 0.005;
    const frameMagic = "LPCM";
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    const state = {
      token: "",
      user: null,
      room: null,
      socket: null,
      audioContext: null,
      stream: null,
      source: null,
      processor: null,
      monitorGain: null,
      sequence: 0,
      streamId: "",
      members: new Map(),
      nextPlaybackTimeByUser: new Map(),
      mutedUserIds: new Set(),
      mutedKeys: new Set(),
      playingSourcesByUser: new Map(),
      playbackGainsByUser: new Map(),
      latencyByUser: new Map(),
      receivedFrames: 0,
      playedFrames: 0,
      mutedFrames: 0,
      droppedFrames: 0,
      lastMuteToggleAtByKey: new Map()
    };

    const roomNameInput = document.getElementById("roomName");
    const usernameInput = document.getElementById("username");
    const inputDeviceSelect = document.getElementById("inputDevice");
    const qualitySelect = document.getElementById("qualitySelect");
    const playbackBufferMsInput = document.getElementById("playbackBufferMs");
    const sendLatencySelect = document.getElementById("sendLatencySelect");
    const refreshDevicesButton = document.getElementById("refreshDevices");
    const joinButton = document.getElementById("joinButton");
    const startButton = document.getElementById("startButton");
    const stopButton = document.getElementById("stopButton");
    const statusEl = document.getElementById("status");
    const membersEl = document.getElementById("members");
    const inputMeter = document.getElementById("inputMeter");
    const transportBadge = document.getElementById("transportBadge");
    const qualitySummary = document.getElementById("qualitySummary");
    const latencyStatsEl = document.getElementById("latencyStats");
    const permissionStatusEl = document.getElementById("permissionStatus");
    const testMicButton = document.getElementById("testMicButton");
    const copyWebUrlButton = document.getElementById("copyWebUrlButton");
    const copyHttpsUrlButton = document.getElementById("copyHttpsUrlButton");
    const copyChromeFlagButton = document.getElementById("copyChromeFlagButton");

    refreshDevicesButton.addEventListener("click", refreshDevices);
    joinButton.addEventListener("click", joinRoom);
    startButton.addEventListener("click", startCapture);
    stopButton.addEventListener("click", stopCapture);
    testMicButton.addEventListener("click", testMicrophonePermission);
    copyWebUrlButton.addEventListener("click", function () { copyText(currentWebUrl(), "已复制当前入口"); });
    copyHttpsUrlButton.addEventListener("click", function () { copyText(httpsWebUrl(), "已复制 HTTPS 入口"); });
    copyChromeFlagButton.addEventListener("click", function () { copyText(chromeInsecureOriginCommand(), "已复制 Chrome/Edge 临时允许命令"); });
    membersEl.addEventListener("click", function (event) {
      const button = findMuteButton(event.target);
      if (!button) {
        return;
      }
      toggleMuteFromButton(button);
    });
    qualitySelect.addEventListener("change", function () {
      updateQualitySummary();
      if (state.user) {
        state.members.set(state.user.id, { userId: state.user.id, username: state.user.username, streamId: state.streamId, format: currentFormat() });
        renderMembers();
      }
    });
    sendLatencySelect.addEventListener("change", function () {
      updateQualitySummary();
      if (state.processor) {
        stopCapture().catch(showError);
        setStatus("发送延迟已切换，点击开始发送重新生效。");
      }
    });
    updateQualitySummary();
    updatePermissionStatus();

    refreshDevices().catch(showError);

    function updatePermissionStatus() {
      const local = ["localhost", "127.0.0.1", "::1"].includes(location.hostname);
      const secure = window.isSecureContext || location.protocol === "https:" || local;
      if (secure) {
        permissionStatusEl.textContent = "当前页面属于安全上下文，浏览器应允许弹出麦克风授权。";
        return;
      }
      permissionStatusEl.textContent = "当前是 HTTP 公网地址，部分浏览器会禁止麦克风。推荐绑定域名和 HTTPS，或在 Chrome/Edge 用临时允许命令测试。";
    }

    async function testMicrophonePermission() {
      try {
        const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
        stream.getTracks().forEach(function (track) { track.stop(); });
        permissionStatusEl.textContent = "麦克风权限可用。";
        await refreshDevices();
      } catch (error) {
        permissionStatusEl.textContent = "麦克风权限不可用：" + (error instanceof Error ? error.message : String(error));
      }
    }

    function currentWebUrl() {
      return location.origin + "/web";
    }

    function httpsWebUrl() {
      return "https://" + location.host + "/web";
    }

    function chromeInsecureOriginCommand() {
      const origin = location.origin;
      return '"%ProgramFiles%\\\\Google\\\\Chrome\\\\Application\\\\chrome.exe" --unsafely-treat-insecure-origin-as-secure="' + origin + '" --user-data-dir="%TEMP%\\\\sonobus-web-mic"\\n"%ProgramFiles(x86)%\\\\Microsoft\\\\Edge\\\\Application\\\\msedge.exe" --unsafely-treat-insecure-origin-as-secure="' + origin + '" --user-data-dir="%TEMP%\\\\sonobus-web-mic-edge"';
    }

    async function copyText(text, message) {
      try {
        await navigator.clipboard.writeText(text);
        permissionStatusEl.textContent = message + "：" + text;
      } catch {
        permissionStatusEl.textContent = "复制失败，请手动复制：" + text;
      }
    }

    async function refreshDevices() {
      const probe = await navigator.mediaDevices.getUserMedia({ audio: true });
      probe.getTracks().forEach(function (track) { track.stop(); });
      const devices = await navigator.mediaDevices.enumerateDevices();
      const inputs = devices.filter(function (device) { return device.kind === "audioinput"; });
      inputDeviceSelect.innerHTML = "";
      for (const device of inputs) {
        const option = document.createElement("option");
        option.value = device.deviceId;
        option.textContent = device.label || "音频输入 " + (inputDeviceSelect.length + 1);
        inputDeviceSelect.appendChild(option);
      }
      setStatus("已刷新输入设备");
    }

    async function joinRoom() {
      await stopCapture();
      const context = ensureAudioContext();
      await context.resume();
      if (state.socket) {
        state.socket.close();
      }
      const response = await fetch("/web/join", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          roomName: roomNameInput.value,
          username: usernameInput.value
        })
      });
      const body = await response.json();
      if (!response.ok) {
        throw new Error(body.error || "加入失败");
      }
      state.token = body.token;
      state.user = body.user;
      state.room = body.room;
      state.members = new Map(body.members.map(function (member) { return [member.userId, member]; }));
      state.members.set(body.user.id, { userId: body.user.id, username: body.user.username, format: currentFormat() });
      transportBadge.textContent = body.transport || "websocket-lpcm";
      renderMembers();

      const scheme = location.protocol === "https:" ? "wss:" : "ws:";
      const socket = new WebSocket(scheme + "//" + location.host + body.streamUrl + "?token=" + encodeURIComponent(body.token));
      socket.binaryType = "arraybuffer";
      socket.onopen = function () {
        startButton.disabled = false;
        setStatus("已加入房间：" + body.room.name + "，正在打开麦克风");
        startCapture().catch(function (error) {
          showError(error);
          startButton.disabled = false;
          stopButton.disabled = true;
        });
      };
      socket.onmessage = function (event) {
        if (typeof event.data === "string") {
          handleServerMessage(JSON.parse(event.data));
        } else {
          handleAudioFrame(event.data);
        }
      };
      socket.onclose = function () {
        startButton.disabled = true;
        stopButton.disabled = true;
        stopCapture().catch(function () {});
        setStatus("连接已断开");
      };
      socket.onerror = function () {
        showError(new Error("WebSocket 连接失败"));
      };
      state.socket = socket;
    }

    function ensureAudioContext() {
      if (!state.audioContext) {
        state.audioContext = new AudioContext({ sampleRate: currentFormat().sampleRate });
      }
      return state.audioContext;
    }

    async function startCapture() {
      if (!state.socket || state.socket.readyState !== WebSocket.OPEN || !state.user) {
        throw new Error("请先加入房间");
      }
      const format = currentFormat();
      const context = ensureAudioContext();
      await context.resume();
      state.stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          deviceId: inputDeviceSelect.value ? { exact: inputDeviceSelect.value } : undefined,
          channelCount: format.channels,
          sampleRate: format.sampleRate,
          echoCancellation: false,
          noiseSuppression: false,
          autoGainControl: false
        }
      });
      state.streamId = state.user.id + "-" + Date.now();
      state.socket.send(JSON.stringify({ type: "stream_format", streamId: state.streamId, format: format }));
      state.source = context.createMediaStreamSource(state.stream);
      const blockSize = sendBlockSize();
      state.processor = context.createScriptProcessor(blockSize, format.channels, format.channels);
      state.monitorGain = context.createGain();
      state.monitorGain.gain.value = 0;
      state.processor.onaudioprocess = function (event) {
        const left = event.inputBuffer.getChannelData(0);
        const right = event.inputBuffer.numberOfChannels > 1 ? event.inputBuffer.getChannelData(1) : left;
        const payload = encodePcmInterleaved(left, right, format);
        const header = {
          streamId: state.streamId,
          userId: state.user.id,
          sampleRate: format.sampleRate,
          bitDepth: format.bitDepth,
          channels: format.channels,
          sequence: state.sequence++,
          timestamp: Date.now()
        };
        updateInputMeter(left, right);
        if (state.socket && state.socket.readyState === WebSocket.OPEN) {
          state.socket.send(encodeAudioFrame(header, payload));
        }
      };
      state.source.connect(state.processor);
      state.processor.connect(state.monitorGain);
      state.monitorGain.connect(context.destination);
      startButton.disabled = true;
      stopButton.disabled = false;
      setStatus("正在发送麦克风音频，发送延迟 " + blockSize + " samples。房间成员同步后可互相听见。");
    }

    async function stopCapture() {
      if (state.processor) {
        state.processor.disconnect();
      }
      if (state.source) {
        state.source.disconnect();
      }
      if (state.monitorGain) {
        state.monitorGain.disconnect();
      }
      if (state.stream) {
        state.stream.getTracks().forEach(function (track) { track.stop(); });
      }
      state.processor = null;
      state.source = null;
      state.monitorGain = null;
      state.stream = null;
      inputMeter.style.width = "0%";
      if (state.socket && state.socket.readyState === WebSocket.OPEN) {
        startButton.disabled = false;
      }
      stopButton.disabled = true;
    }

    function handleServerMessage(message) {
      if (message.type === "room_state") {
        state.members = new Map(message.members.map(function (member) { return [member.userId, member]; }));
        if (state.user) {
          state.members.set(state.user.id, { userId: state.user.id, username: state.user.username, streamId: state.streamId, format: currentFormat() });
        }
        renderMembers();
      } else if (message.type === "member_joined") {
        state.members.set(message.member.userId, message.member);
        renderMembers();
      } else if (message.type === "member_left") {
        state.members.delete(message.userId);
        renderMembers();
      } else if (message.type === "stream_format") {
        state.members.set(message.member.userId, message.member);
        renderMembers();
      } else if (message.type === "error") {
        showError(new Error(message.message));
      }
    }

    function handleAudioFrame(raw) {
      const frame = decodeAudioFrame(raw);
      if (state.user && frame.header.userId === state.user.id) {
        return;
      }
      const previous = state.members.get(frame.header.userId) || { userId: frame.header.userId, username: frame.header.userId.slice(0, 8) };
      state.members.set(frame.header.userId, {
        userId: frame.header.userId,
        username: previous.username || frame.header.userId.slice(0, 8),
        streamId: frame.header.streamId,
        format: {
          sampleRate: frame.header.sampleRate,
          bitDepth: frame.header.bitDepth,
          channels: frame.header.channels
        }
      });
      renderMembers();
      state.receivedFrames += 1;
      if (isMutedFrame(frame)) {
        state.mutedFrames += 1;
        updateLatency(frame.header.userId, frame.header.timestamp, 0);
        return;
      }
      playPcmFrame(frame);
    }

    function encodeAudioFrame(header, payload) {
      const headerBytes = encoder.encode(JSON.stringify(header));
      const output = new ArrayBuffer(12 + headerBytes.byteLength + payload.byteLength);
      const view = new DataView(output);
      writeAscii(view, 0, frameMagic);
      view.setUint8(4, 1);
      view.setUint8(5, 1);
      view.setUint16(6, headerBytes.byteLength, false);
      view.setUint32(8, payload.byteLength, false);
      new Uint8Array(output, 12, headerBytes.byteLength).set(headerBytes);
      new Uint8Array(output, 12 + headerBytes.byteLength).set(payload);
      return output;
    }

    function decodeAudioFrame(raw) {
      const bytes = new Uint8Array(raw);
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (readAscii(view, 0, 4) !== frameMagic || view.getUint8(4) !== 1 || view.getUint8(5) !== 1) {
        throw new Error("远端音频帧格式不支持");
      }
      const headerLength = view.getUint16(6, false);
      const payloadLength = view.getUint32(8, false);
      const headerStart = 12;
      const payloadStart = headerStart + headerLength;
      if (bytes.byteLength !== payloadStart + payloadLength) {
        throw new Error("远端音频帧长度不匹配");
      }
      const header = JSON.parse(decoder.decode(bytes.slice(headerStart, payloadStart)));
      const payload = bytes.slice(payloadStart);
      return { header: header, payload: payload };
    }

    function playPcmFrame(frame) {
      const context = ensureAudioContext();
      if (context.state === "suspended") {
        latencyStatsEl.textContent = "浏览器暂停了音频播放，点击页面后重试，或点击开始发送。";
        state.droppedFrames += 1;
        renderLatencyStats();
        return;
      }
      const channelCount = frame.header.channels || 1;
      const bytesPerSample = Math.ceil((frame.header.bitDepth || 16) / 8);
      const samplesPerChannel = Math.floor(frame.payload.byteLength / bytesPerSample / channelCount);
      if (samplesPerChannel <= 0) {
        state.droppedFrames += 1;
        renderLatencyStats();
        return;
      }
      const buffer = context.createBuffer(channelCount, samplesPerChannel, frame.header.sampleRate);
      const view = new DataView(frame.payload.buffer, frame.payload.byteOffset, frame.payload.byteLength);
      for (let frameIndex = 0; frameIndex < samplesPerChannel; frameIndex += 1) {
        for (let channel = 0; channel < channelCount; channel += 1) {
          const byteOffset = (frameIndex * channelCount + channel) * bytesPerSample;
          const sample = readPcmSample(view, byteOffset, frame.header.bitDepth || 16);
          buffer.getChannelData(channel)[frameIndex] = sample;
        }
      }
      const source = context.createBufferSource();
      source.buffer = buffer;
      source.connect(playbackGainForUser(frame.header.userId));
      rememberPlayingSource(frame.header.userId, source);
      source.onended = function () {
        forgetPlayingSource(frame.header.userId, source);
      };
      const leadSeconds = playbackBufferMs() / 1000;
      const minLeadSeconds = Math.min(playbackMinLeadSeconds, leadSeconds);
      const next = state.nextPlaybackTimeByUser.get(frame.header.userId) || context.currentTime + leadSeconds;
      const startAt = Math.max(context.currentTime + minLeadSeconds, next);
      source.start(startAt);
      state.nextPlaybackTimeByUser.set(frame.header.userId, startAt + buffer.duration);
      state.playedFrames += 1;
      updateLatency(frame.header.userId, frame.header.timestamp, Math.max(0, startAt - context.currentTime) * 1000);
    }

    function encodePcmInterleaved(left, right, format) {
      const length = Math.min(left.length, right.length);
      const bytesPerSample = Math.ceil(format.bitDepth / 8);
      const output = new Uint8Array(length * format.channels * bytesPerSample);
      const view = new DataView(output.buffer);
      for (let i = 0; i < length; i += 1) {
        const channelValues = [left[i] || 0, right[i] || left[i] || 0];
        for (let channel = 0; channel < format.channels; channel += 1) {
          writePcmSample(view, (i * format.channels + channel) * bytesPerSample, channelValues[channel] || channelValues[0], format.bitDepth);
        }
      }
      return output;
    }

    function writePcmSample(view, byteOffset, value, bitDepth) {
      const sample = Math.max(-1, Math.min(1, value || 0));
      if (bitDepth === 16) {
        view.setInt16(byteOffset, Math.round(sample * 32767), true);
        return;
      }
      const intValue = Math.max(-8388608, Math.min(8388607, Math.round(sample * 8388607)));
      view.setUint8(byteOffset, intValue & 255);
      view.setUint8(byteOffset + 1, (intValue >> 8) & 255);
      view.setUint8(byteOffset + 2, (intValue >> 16) & 255);
    }

    function readPcmSample(view, byteOffset, bitDepth) {
      if (bitDepth === 24) {
        let value = view.getUint8(byteOffset) | (view.getUint8(byteOffset + 1) << 8) | (view.getUint8(byteOffset + 2) << 16);
        if (value & 0x800000) {
          value |= ~0xffffff;
        }
        return Math.max(-1, Math.min(1, value / 8388608));
      }
      if (bitDepth === 32) {
        return Math.max(-1, Math.min(1, view.getInt32(byteOffset, true) / 2147483648));
      }
      return view.getInt16(byteOffset, true) / 32768;
    }

    function updateInputMeter(left, right) {
      let peak = 0;
      const length = Math.min(left.length, right.length);
      for (let i = 0; i < length; i += 1) {
        peak = Math.max(peak, Math.abs(left[i] || 0), Math.abs(right[i] || 0));
      }
      inputMeter.style.width = Math.min(100, Math.round(peak * 140)) + "%";
    }

    function muteKeysForMember(member) {
      return [member.userId, member.username, member.streamId].filter(Boolean).map(String);
    }

    function isMutedMember(member) {
      return muteKeysForMember(member).some(function (key) { return state.mutedKeys.has(key); });
    }

    function isMutedFrame(frame) {
      const member = state.members.get(frame.header.userId);
      const keys = [frame.header.userId, frame.header.streamId, member && member.username, member && member.streamId].filter(Boolean).map(String);
      return keys.some(function (key) { return state.mutedKeys.has(key); });
    }

    function toggleMute(member) {
      const keys = muteKeysForMember(member);
      const userId = member.userId;
      if (isMutedMember(member)) {
        for (const key of keys) {
          state.mutedKeys.delete(key);
        }
        state.mutedUserIds.delete(userId);
        setUserGain(userId, 1);
        setStatus("已取消静音：" + (member.username || userId));
      } else {
        for (const key of keys) {
          state.mutedKeys.add(key);
        }
        state.mutedUserIds.add(userId);
        state.nextPlaybackTimeByUser.delete(userId);
        setUserGain(userId, 0);
        stopPlayingSources(userId);
        setStatus("已静音：" + (member.username || userId));
      }
      renderMembers();
    }

    function toggleMuteFromButton(button) {
      const member = {
        userId: button.getAttribute("data-mute-user-id") || button.getAttribute("data-user-id") || "",
        username: button.getAttribute("data-mute-username") || button.getAttribute("data-username") || "",
        streamId: button.getAttribute("data-mute-stream-id") || button.getAttribute("data-stream-id") || ""
      };
      if (!member.userId) {
        return;
      }
      const now = Date.now();
      const last = state.lastMuteToggleAtByKey.get(member.userId) || 0;
      if (now - last < 300) {
        return;
      }
      state.lastMuteToggleAtByKey.set(member.userId, now);
      toggleMute(member);
    }

    function findMuteButton(target) {
      let node = target;
      while (node && node !== membersEl) {
        if (node.tagName === "BUTTON" && node.getAttribute("data-mute-user-id")) {
          return node;
        }
        node = node.parentNode;
      }
      return null;
    }

    function playbackGainForUser(userId) {
      let gain = state.playbackGainsByUser.get(userId);
      if (!gain) {
        gain = ensureAudioContext().createGain();
        const member = state.members.get(userId);
        gain.gain.value = member && isMutedMember(member) ? 0 : 1;
        gain.connect(ensureAudioContext().destination);
        state.playbackGainsByUser.set(userId, gain);
      }
      return gain;
    }

    function setUserGain(userId, value) {
      const gain = playbackGainForUser(userId);
      const context = ensureAudioContext();
      gain.gain.cancelScheduledValues(context.currentTime);
      gain.gain.setValueAtTime(value, context.currentTime);
    }

    function rememberPlayingSource(userId, source) {
      let sources = state.playingSourcesByUser.get(userId);
      if (!sources) {
        sources = new Set();
        state.playingSourcesByUser.set(userId, sources);
      }
      sources.add(source);
    }

    function forgetPlayingSource(userId, source) {
      const sources = state.playingSourcesByUser.get(userId);
      if (!sources) {
        return;
      }
      sources.delete(source);
      if (!sources.size) {
        state.playingSourcesByUser.delete(userId);
      }
    }

    function stopPlayingSources(userId) {
      const sources = state.playingSourcesByUser.get(userId);
      if (!sources) {
        return;
      }
      for (const source of Array.from(sources)) {
        try {
          source.stop();
        } catch {}
        source.disconnect();
      }
      state.playingSourcesByUser.delete(userId);
    }

    function playbackBufferMs() {
      return Math.max(5, Math.min(200, Number(playbackBufferMsInput.value || 25)));
    }

    function sendBlockSize() {
      const value = Number(sendLatencySelect.value || 512);
      return [256, 512, 1024, 2048, 4096].includes(value) ? value : 512;
    }

    function currentFormat() {
      const parts = String(qualitySelect.value || "48000-24-2").split("-").map(Number);
      return {
        sampleRate: parts[0] || defaultFormat.sampleRate,
        bitDepth: parts[1] === 16 ? 16 : 24,
        channels: parts[2] === 1 ? 1 : 2
      };
    }

    function updateQualitySummary() {
      const format = currentFormat();
      qualitySummary.textContent = "采样率 " + format.sampleRate + "Hz，" + format.bitDepth + "bit，" + (format.channels === 2 ? "双声道" : "单声道") + "，发送 " + sendBlockSize() + " samples。浏览器需要允许麦克风权限。";
    }

    function updateLatency(userId, sentAt, scheduledMs) {
      const networkMs = Number.isFinite(sentAt) ? Date.now() - sentAt : NaN;
      const usableNetworkMs = networkMs >= -1000 && networkMs < 10000 ? Math.max(0, networkMs) : NaN;
      state.latencyByUser.set(userId, {
        networkMs: usableNetworkMs,
        scheduledMs: Math.max(0, Math.round(scheduledMs || 0)),
        totalMs: Number.isFinite(usableNetworkMs) ? Math.max(0, Math.round(usableNetworkMs + (scheduledMs || 0))) : undefined,
        updatedAt: Date.now()
      });
      renderLatencyStats();
    }

    function renderLatencyStats() {
      const rows = [];
      for (const [userId, latency] of state.latencyByUser.entries()) {
        if (Date.now() - latency.updatedAt > 5000) {
          continue;
        }
        const member = state.members.get(userId);
        const name = member ? member.username : userId.slice(0, 8);
        const network = Number.isFinite(latency.networkMs) ? Math.round(latency.networkMs) + "ms" : "时钟不同步";
        const total = latency.totalMs === undefined ? "未知" : latency.totalMs + "ms";
        rows.push(name + "：估算 " + total + "，网络/处理 " + network + "，播放缓冲 " + latency.scheduledMs + "ms");
      }
      const counters = "收到 " + state.receivedFrames + "，播放 " + state.playedFrames + "，静音丢弃 " + state.mutedFrames + "，未播放 " + state.droppedFrames;
      latencyStatsEl.textContent = rows.length ? rows.join("；") + "。" + counters : "未收到远端音频。" + counters;
    }

    function renderMembers() {
      const members = Array.from(state.members.values());
      if (!members.length) {
        membersEl.innerHTML = '<div class="empty">没有在线成员。</div>';
        return;
      }
      membersEl.innerHTML = "";
      for (const member of members) {
        const row = document.createElement("div");
        row.className = "member";
        const info = document.createElement("div");
        const name = document.createElement("strong");
        name.textContent = member.username || member.userId;
        const details = document.createElement("small");
        details.textContent = member.format
          ? member.format.sampleRate + "Hz / " + member.format.bitDepth + "bit / " + member.format.channels + "ch"
          : (member.userId && String(member.userId).startsWith("sonobus-") ? "等待原生客户端音频" : "已加入，未开麦");
        info.appendChild(name);
        info.appendChild(details);
        const badge = document.createElement("span");
        badge.className = "badge";
        const isMe = state.user && member.userId === state.user.id;
        badge.textContent = isMe ? "我" : "远端";
        row.appendChild(info);
        row.appendChild(badge);
        if (!isMe) {
          const muted = isMutedMember(member);
          const muteButton = document.createElement("button");
          muteButton.type = "button";
          muteButton.textContent = muted ? "取消静音" : "静音";
          muteButton.className = muted ? "active" : "";
          muteButton.setAttribute("data-mute-user-id", member.userId || "");
          muteButton.setAttribute("data-user-id", member.userId || "");
          muteButton.setAttribute("data-mute-username", member.username || "");
          muteButton.setAttribute("data-username", member.username || "");
          muteButton.setAttribute("data-mute-stream-id", member.streamId || "");
          muteButton.setAttribute("data-stream-id", member.streamId || "");
          muteButton.onclick = function () { toggleMuteFromButton(muteButton); };
          muteButton.addEventListener("pointerup", function () { toggleMuteFromButton(muteButton); });
          muteButton.addEventListener("mouseup", function () { toggleMuteFromButton(muteButton); });
          muteButton.addEventListener("touchend", function () { toggleMuteFromButton(muteButton); });
          row.appendChild(muteButton);
        }
        membersEl.appendChild(row);
      }
    }

    function writeAscii(view, offset, value) {
      for (let i = 0; i < value.length; i += 1) {
        view.setUint8(offset + i, value.charCodeAt(i));
      }
    }

    function readAscii(view, offset, length) {
      let value = "";
      for (let i = 0; i < length; i += 1) {
        value += String.fromCharCode(view.getUint8(offset + i));
      }
      return value;
    }

    function setStatus(text) {
      statusEl.className = "status";
      statusEl.textContent = text;
    }

    function showError(error) {
      statusEl.className = "status error";
      statusEl.textContent = error instanceof Error ? error.message : String(error);
    }
  </script>
</body>
</html>`;


const adminPageHtml = String.raw`<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>服务器管理</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif;
      background: #111418;
      color: #edf1f5;
    }
    body {
      margin: 0;
      background: #111418;
    }
    main {
      max-width: 1480px;
      margin: 0 auto;
      padding: 24px;
      box-sizing: border-box;
    }
    h1 {
      font-size: 24px;
      margin: 0;
      letter-spacing: 0;
    }
    .page-head {
      display: flex;
      justify-content: space-between;
      gap: 16px;
      align-items: center;
      margin-bottom: 18px;
    }
    .badge, .state-badge {
      display: inline-flex;
      align-items: center;
      width: fit-content;
      min-height: 26px;
      padding: 0 10px;
      border: 1px solid #315b3d;
      border-radius: 999px;
      background: #132018;
      color: #a7e0b5;
      font-size: 12px;
      font-weight: 600;
      white-space: nowrap;
    }
    .state-badge.waiting {
      border-color: #72561f;
      background: #261f12;
      color: #f2cd78;
    }
    .state-badge.offline, .state-badge.neutral {
      border-color: #46515d;
      background: #1b2229;
      color: #b8c2cc;
    }
    h2 {
      font-size: 17px;
      margin: 0 0 12px;
      letter-spacing: 0;
    }
    section {
      border-top: 1px solid #303842;
      padding: 18px 0;
    }
    .row {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      align-items: end;
    }
    label {
      display: grid;
      gap: 6px;
      color: #b8c2cc;
      font-size: 13px;
    }
    input, select, button {
      font: inherit;
      min-height: 36px;
      max-width: 100%;
      border-radius: 6px;
      border: 1px solid #3a4550;
      background: #191f26;
      color: #edf1f5;
      padding: 0 10px;
      box-sizing: border-box;
    }
    input {
      min-width: 180px;
    }
    input.small {
      width: 96px;
      min-width: 96px;
    }
    #baseUrl {
      width: 260px;
    }
    button {
      cursor: pointer;
      background: #225d8f;
      border-color: #2d77b4;
      font-weight: 600;
      white-space: nowrap;
    }
    button:hover {
      filter: brightness(1.08);
    }
    button.secondary {
      background: #28313a;
      border-color: #3a4550;
    }
    button.danger {
      background: #8d2d2d;
      border-color: #b23a3a;
    }
    button.warning {
      background: #8a5a18;
      border-color: #b7791f;
    }
    button:disabled {
      opacity: .55;
      cursor: not-allowed;
    }
    .table-wrap {
      width: 100%;
      overflow-x: auto;
      margin-top: 12px;
      border: 1px solid #2b333c;
      border-radius: 6px;
    }
    table {
      width: 100%;
      min-width: 720px;
      border-collapse: collapse;
      font-size: 13px;
    }
    th, td {
      border-bottom: 1px solid #2b333c;
      padding: 10px;
      text-align: left;
      vertical-align: top;
    }
    th {
      color: #b8c2cc;
      font-weight: 600;
    }
    td {
      overflow-wrap: anywhere;
    }
    .connections-table {
      min-width: 1080px;
      table-layout: fixed;
    }
    .connections-table th:nth-child(1) { width: 17%; }
    .connections-table th:nth-child(2) { width: 21%; }
    .connections-table th:nth-child(3) { width: 13%; }
    .connections-table th:nth-child(4) { width: 32%; }
    .connections-table th:nth-child(5) { width: 17%; }
    .connections-table td {
      max-width: none;
      overflow: visible;
      white-space: normal;
      text-overflow: clip;
    }
    .member-meta, .connection-meta, .relay-stats, .camera-actions {
      display: grid;
      gap: 5px;
      min-width: 0;
    }
    .member-meta strong {
      color: #edf1f5;
      font-size: 14px;
    }
    .meta-line, .camera-help, .camera-details {
      color: #95a0aa;
      line-height: 1.4;
      overflow-wrap: anywhere;
    }
    .connection-head {
      display: flex;
      align-items: center;
      flex-wrap: wrap;
      gap: 7px;
    }
    .connection-head strong {
      min-width: 0;
    }
    .relay-stats {
      line-height: 1.3;
    }
    .relay-stats span {
      display: block;
    }
    .actions {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
      align-items: flex-start;
    }
    .connection-actions button {
      flex: 1 1 92px;
    }
    .camera-control-row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
      align-items: center;
    }
    .camera-select {
      flex: 1 1 220px;
      width: 100%;
      min-width: 0;
    }
    .camera-control-row button {
      flex: 0 0 auto;
    }
    .camera-actions > button {
      justify-self: start;
    }
    .toolbar {
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding: 10px;
      background: #111820;
      border: 1px solid #2b333c;
      border-radius: 6px;
    }
    .toolbar .row {
      gap: 10px;
    }
    .toolbar label {
      display: grid;
      gap: 4px;
      color: #b8c2cc;
      font-size: 12px;
    }
    .toolbar label span {
      color: #95a0aa;
    }
    .status {
      min-height: 22px;
      margin-top: 10px;
      color: #a7d7ff;
      font-size: 13px;
      line-height: 1.45;
      white-space: pre-wrap;
      overflow-wrap: anywhere;
    }
    .status.ok {
      color: #a7e0b5;
    }
    .subtle {
      color: #95a0aa;
      font-size: 12px;
      margin-top: 6px;
    }
    .error {
      color: #ffb2b2;
    }
    .muted {
      color: #95a0aa;
    }
    @media (max-width: 1100px) {
      .connections-wrap {
        overflow: visible;
        border: 0;
      }
      .connections-table, .connections-table tbody {
        display: block;
        min-width: 0;
      }
      .connections-table thead {
        display: none;
      }
      .connections-table tr {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
        gap: 12px 18px;
        border: 1px solid #2b333c;
        border-radius: 6px;
        margin: 10px 0;
        padding: 12px;
      }
      .connections-table td {
        display: block;
        border: 0;
        padding: 0;
      }
      .connections-table td::before {
        content: attr(data-label);
        display: block;
        margin-bottom: 5px;
        color: #95a0aa;
        font-size: 12px;
      }
      .connections-table td[data-label="摄像头"],
      .connections-table td[data-label="操作"],
      .connections-table td[colspan] {
        grid-column: 1 / -1;
      }
    }
    @media (max-width: 760px) {
      main {
        padding: 16px;
      }
      input, input.small, #baseUrl {
        width: 100%;
        min-width: 0;
      }
      .page-head {
        display: block;
      }
      .badge {
        margin-top: 10px;
      }
      .row label {
        width: 100%;
      }
      .toolbar {
        align-items: stretch;
      }
      .toolbar .row {
        width: 100%;
      }
      .toolbar button {
        width: 100%;
      }
      .bans-wrap {
        overflow: visible;
        border: 0;
      }
      .bans-table, .bans-table tbody {
        display: block;
        min-width: 0;
      }
      .bans-table thead {
        display: none;
      }
      .bans-table tr {
        display: block;
        border: 1px solid #2b333c;
        border-radius: 6px;
        margin: 10px 0;
        padding: 8px;
      }
      .bans-table td {
        display: block;
        border: 0;
        padding: 6px 0;
      }
      .bans-table td::before {
        content: attr(data-label);
        display: block;
        color: #95a0aa;
        font-size: 12px;
      }
    }
    @media (max-width: 640px) {
      .connections-table tr {
        grid-template-columns: minmax(0, 1fr);
      }
      .connections-table td[data-label="摄像头"],
      .connections-table td[data-label="操作"] {
        grid-column: auto;
      }
      .camera-control-row button, .connection-actions button {
        width: 100%;
        flex-basis: 100%;
      }
    }
  </style>
</head>
<body>
  <main>
    <div class="page-head">
      <h1>服务器管理</h1>
      <span class="badge">自建 SonoBus 中继</span>
    </div>

    <section>
      <h2>管理员登录</h2>
      <div class="row">
        <label>服务器地址
          <input id="baseUrl" autocomplete="url">
        </label>
        <label>管理员账号
          <input id="username" autocomplete="username" value="admin">
        </label>
        <label>管理员密码
          <input id="password" type="password" autocomplete="current-password">
        </label>
        <button id="loginBtn">登录</button>
        <button class="secondary" id="logoutBtn">退出</button>
      </div>
      <div id="loginStatus" class="status"></div>
    </section>

    <section>
      <h2>在线连接</h2>
      <div class="row toolbar">
        <div class="row">
          <button id="refreshBtn">刷新</button>
          <label>封禁时长
            <select id="banSeconds">
              <option value="600">10 分钟</option>
              <option value="3600" selected>1 小时</option>
              <option value="86400">1 天</option>
              <option value="custom">自定义</option>
              <option value="0">永久</option>
            </select>
          </label>
          <label id="customBanLabel">自定义分钟
            <input id="customBanMinutes" class="small" type="number" min="1" max="43200" value="60">
            <span>最长 43200 分钟</span>
          </label>
        </div>
        <span id="connectionSummary" class="muted">未刷新</span>
      </div>
      <div id="connectionStatus" class="status"></div>
      <div class="table-wrap connections-wrap">
        <table class="connections-table">
          <thead>
            <tr>
              <th>成员</th>
              <th>连接</th>
              <th>中继</th>
              <th>摄像头</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody id="connectionsBody">
            <tr><td colspan="5" class="muted">登录后点击刷新。</td></tr>
          </tbody>
        </table>
      </div>
    </section>

    <section>
      <h2>封禁列表</h2>
      <div class="row">
        <button id="refreshBansBtn">刷新封禁</button>
      </div>
      <div class="subtle">误封后可在这里解除。封禁保存在数据库里，Docker 重启后会自动恢复。</div>
      <div id="banStatus" class="status"></div>
      <div class="table-wrap bans-wrap">
        <table class="bans-table">
          <thead>
            <tr>
              <th>类型</th>
              <th>房间/群组</th>
              <th>用户</th>
              <th>IP</th>
              <th>到期时间</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody id="bansBody">
            <tr><td colspan="6" class="muted">登录后点击刷新封禁。</td></tr>
          </tbody>
        </table>
      </div>
    </section>
  </main>

  <script>
    const tokenKey = "lossless-admin-token";
    const baseUrlInput = document.getElementById("baseUrl");
    const usernameInput = document.getElementById("username");
    const passwordInput = document.getElementById("password");
    const loginStatus = document.getElementById("loginStatus");
    const connectionStatus = document.getElementById("connectionStatus");
    const connectionSummary = document.getElementById("connectionSummary");
    const banStatus = document.getElementById("banStatus");
    const body = document.getElementById("connectionsBody");
    const bansBody = document.getElementById("bansBody");
    const banSeconds = document.getElementById("banSeconds");
    const customBanLabel = document.getElementById("customBanLabel");
    const customBanMinutes = document.getElementById("customBanMinutes");
    let currentVideoControls = [];
    let currentVideoUrls = {};
    let connectionRefreshPromise;
    let lastConnectionsFingerprint = "";
    let authGeneration = 0;

    baseUrlInput.value = location.origin;

    document.getElementById("loginBtn").addEventListener("click", login);
    document.getElementById("logoutBtn").addEventListener("click", logout);
    document.getElementById("refreshBtn").addEventListener("click", () => refreshConnections());
    document.getElementById("refreshBansBtn").addEventListener("click", refreshBans);
    banSeconds.addEventListener("change", updateBanDurationControls);
    updateBanDurationControls();
    setInterval(() => {
      if (localStorage.getItem(tokenKey) && document.visibilityState === "visible") refreshConnections(true);
    }, 3000);

    if (localStorage.getItem(tokenKey)) {
      setStatus(loginStatus, "已保存登录状态。");
      refreshConnections();
      refreshBans();
    }

    async function login() {
      const generation = ++authGeneration;
      setStatus(loginStatus, "正在登录...");
      try {
        const response = await fetch(apiUrl("/auth/login"), {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ username: usernameInput.value.trim(), password: passwordInput.value })
        });
        const data = await response.json();
        if (authGeneration !== generation) return;
        if (!response.ok) throw new Error(data.error || "登录失败");
        localStorage.setItem(tokenKey, data.token);
        lastConnectionsFingerprint = "";
        passwordInput.value = "";
        setStatus(loginStatus, "登录成功。", false, true);
        await refreshConnections();
        await refreshBans();
      } catch (error) {
        if (authGeneration === generation) setStatus(loginStatus, error.message, true);
      }
    }

    function logout() {
      authGeneration += 1;
      localStorage.removeItem(tokenKey);
      lastConnectionsFingerprint = "";
      body.innerHTML = '<tr><td colspan="5" class="muted">已退出。</td></tr>';
      bansBody.innerHTML = '<tr><td colspan="6" class="muted">已退出。</td></tr>';
      connectionSummary.textContent = "未刷新";
      setStatus(loginStatus, "已退出。");
    }

    async function refreshConnections(quiet = false) {
      if (connectionRefreshPromise) {
        const activeRefresh = connectionRefreshPromise;
        await activeRefresh;
        if (!quiet) {
          if (connectionRefreshPromise === activeRefresh) connectionRefreshPromise = undefined;
          return refreshConnections(false);
        }
        return;
      }

      const token = localStorage.getItem(tokenKey);
      const generation = authGeneration;
      if (!token) {
        if (!quiet) setStatus(connectionStatus, "请先登录。", true);
        return;
      }

      const request = (async () => {
        if (!quiet) setStatus(connectionStatus, "正在刷新...");
        try {
          const data = await apiGet("/admin/connections");
          if (authGeneration !== generation || localStorage.getItem(tokenKey) !== token) return;

          const connections = data.connections || [];
          currentVideoControls = data.videoControls || [];
          currentVideoUrls = data.videoUrls || {};
          const fingerprint = JSON.stringify([connections, currentVideoControls, currentVideoUrls]);
          const interacting = body.contains(document.activeElement);
          if (!quiet || (!interacting && fingerprint !== lastConnectionsFingerprint)) {
            renderConnections(connections);
            lastConnectionsFingerprint = fingerprint;
          }
          const onlineCount = connections.filter((connection) => connection.online !== false).length;
          connectionSummary.textContent = "在线 " + onlineCount + " 个 / 总计 " + connections.length + " 个";
          if (!quiet) {
            const relayDiagnostic = displayRelayDiagnostic(data.diagnostics && data.diagnostics.udpRelay);
            setStatus(connectionStatus, "已刷新：" + new Date().toLocaleString() + relayDiagnostic, false, true);
          }
        } catch (error) {
          if (authGeneration === generation && localStorage.getItem(tokenKey) === token) setStatus(connectionStatus, error.message, true);
        }
      })();

      connectionRefreshPromise = request;
      try {
        await request;
      } finally {
        if (connectionRefreshPromise === request) connectionRefreshPromise = undefined;
      }
    }

    async function kick(connection) {
      if (!confirm("确定踢出 " + displayUser(connection) + " 吗？")) return;
      const session = captureAdminSession();
      if (!adminSessionCurrent(session)) return;
      const result = await apiPost("/admin/connections/kick", connectionPayload(connection));
      if (!adminSessionCurrent(session)) return;
      await refreshConnections();
      if (!adminSessionCurrent(session)) return;
      await refreshBans();
      if (!adminSessionCurrent(session)) return;
      setStatus(connectionStatus, "已踢出 " + (result.kicked || 0) + " 条当前记录。UDP 客户端如果还在连接，会在继续发包后重新出现；要阻止它回来请点封禁。");
    }

    async function ban(connection) {
      if (!confirm("确定踢出并封禁 " + displayUser(connection) + " 吗？")) return;
      const session = captureAdminSession();
      if (!adminSessionCurrent(session)) return;
      const ttlSeconds = selectedBanSeconds();
      const result = await apiPost("/admin/bans", { ...connectionPayload(connection), ttlSeconds });
      if (!adminSessionCurrent(session)) return;
      await refreshConnections();
      if (!adminSessionCurrent(session)) return;
      await refreshBans();
      if (!adminSessionCurrent(session)) return;
      setStatus(connectionStatus, "已封禁，踢出 " + (result.banned || 0) + " 条连接，到期时间：" + displayExpiresAt(result.expiresAt), false, true);
    }

    function updateBanDurationControls() {
      customBanLabel.style.display = banSeconds.value === "custom" ? "grid" : "none";
    }

    function selectedBanSeconds() {
      if (banSeconds.value === "custom") {
        const minutes = Math.max(1, Math.min(Number(customBanMinutes.value || 1), 43200));
        customBanMinutes.value = String(minutes);
        return minutes * 60;
      }
      return Number(banSeconds.value);
    }

    function captureAdminSession() {
      return { generation: authGeneration, token: localStorage.getItem(tokenKey) };
    }

    function adminSessionCurrent(session) {
      return Boolean(session.token)
        && authGeneration === session.generation
        && localStorage.getItem(tokenKey) === session.token;
    }

    async function refreshBans() {
      const token = localStorage.getItem(tokenKey);
      const generation = authGeneration;
      if (!token) {
        setStatus(banStatus, "请先登录。", true);
        return;
      }
      setStatus(banStatus, "正在刷新封禁...");
      try {
        const data = await apiGet("/admin/bans");
        if (authGeneration !== generation || localStorage.getItem(tokenKey) !== token) return;
        renderBans(data.bans || []);
        setStatus(banStatus, "已刷新封禁：" + new Date().toLocaleString() + "，共 " + (data.bans || []).length + " 条。", false, true);
      } catch (error) {
        if (authGeneration === generation && localStorage.getItem(tokenKey) === token) setStatus(banStatus, error.message, true);
      }
    }

    function renderConnections(connections) {
      if (!connections.length) {
        body.innerHTML = '<tr><td colspan="5" class="muted">当前没有在线连接。</td></tr>';
        connectionSummary.textContent = "在线 0 个";
        return;
      }
      body.innerHTML = "";
      for (const connection of connections) {
        const tr = document.createElement("tr");
        const room = connection.group || connection.roomId || "-";
        const user = connection.user || connection.username || connection.userId || "-";
        const address = connection.address || "-";
        const port = connection.port || "-";
        const lastSeen = connection.lastSeenAt || connection.joinedAt || connection.createdAt || "-";
        const online = connection.online !== false;
        const systemBridge = isSystemBridge(connection);
        const videoRoom = systemBridge ? "" : (connection.type === "sonobus-connection" && room !== "-"
          ? (room.startsWith("SB_") ? room : "SB_" + room)
          : "");
        const control = connection.type === "sonobus-connection"
          ? currentVideoControls.find((candidate) => candidate.group === room && candidate.user === user)
          : undefined;
        const relayStats = displayRelayStats(connection);
        tr.innerHTML =
          memberCell(room, user, systemBridge) +
          connectionCell(connection, address, port, lastSeen, online) +
          relayCell(relayStats) +
          '<td data-label="摄像头"><div class="camera-actions"></div></td>' +
          '<td data-label="操作"><div class="actions connection-actions"></div></td>';
        const cameraActions = tr.querySelector(".camera-actions");
        const actions = tr.querySelector(".connection-actions");
        renderCameraControl(cameraActions, connection, control, videoRoom, systemBridge);
        if (!systemBridge) {
          if (videoRoom) {
            const videoConnection = { ...connection, videoRoom };
            const videoButton = document.createElement("button");
            videoButton.className = "secondary";
            videoButton.textContent = "打开视频";
            videoButton.title = "用 WebRTC 打开该群组的 H.264 + Opus 直播";
            videoButton.addEventListener("click", () => openVideo(videoConnection));
            actions.appendChild(videoButton);

            const obsButton = document.createElement("button");
            obsButton.className = "secondary";
            obsButton.textContent = "复制 OBS 地址";
            obsButton.title = "复制 OBS 媒体源 RTSP 地址";
            obsButton.addEventListener("click", () => copyObsUrl(videoConnection));
            actions.appendChild(obsButton);
          }
          const kickButton = document.createElement("button");
          kickButton.className = "danger";
          kickButton.textContent = "踢出";
          kickButton.title = "只删除当前在线记录；UDP 客户端继续发包会重新出现";
          kickButton.addEventListener("click", () => runAction(() => kick(connection)));
          actions.appendChild(kickButton);

          if (connection.type !== "websocket") {
            const banButton = document.createElement("button");
            banButton.className = "warning";
            banButton.textContent = "封禁";
            banButton.title = "踢出并按上方时长封禁";
            banButton.addEventListener("click", () => runAction(() => ban(connection)));
            actions.appendChild(banButton);
          }
        }
        body.appendChild(tr);
      }
    }

    function renderCameraControl(container, connection, control, videoRoom, systemBridge) {
      if (systemBridge || connection.type !== "sonobus-connection" || !videoRoom) {
        container.textContent = "-";
        return;
      }
      const state = document.createElement("span");
      state.className = "state-badge";
      const help = document.createElement("span");
      help.className = "camera-help";
      container.appendChild(state);
      container.appendChild(help);

      if (!control) {
        state.classList.add("waiting");
        state.textContent = "未配对";
        help.textContent = "设备列表不可用；等待生成配对信息。";
        const pairButton = document.createElement("button");
        pairButton.className = "secondary";
        pairButton.textContent = "生成配对码";
        pairButton.title = "生成一次性配对信息";
        pairButton.addEventListener("click", () => runAction(() => pairCamera(connection)));
        container.appendChild(pairButton);
        return;
      }

      const cameras = control.cameras || [];
      if (!control.online) {
        state.classList.add("offline");
        state.textContent = "客户端离线";
        help.textContent = "等待新版客户端完成配对并上线。";
      } else if (!cameras.length) {
        state.classList.add("waiting");
        state.textContent = "在线 · 暂无摄像头";
        help.textContent = "客户端未上报设备；检查摄像头权限或客户端版本。";
      } else {
        state.textContent = "在线 · " + cameras.length + " 台摄像头";
        help.textContent = "设备列表已同步。";
      }

      const controls = document.createElement("div");
      controls.className = "camera-control-row";
      const select = document.createElement("select");
      select.className = "camera-select";
      const automatic = document.createElement("option");
      automatic.value = "";
      automatic.textContent = cameras.length
        ? "自动选择最高 60 FPS"
        : control.online ? "自动选择（等待摄像头）" : "自动选择（客户端离线）";
      select.appendChild(automatic);
      for (const camera of cameras) {
        const option = document.createElement("option");
        option.value = camera.id;
        option.textContent = camera.name;
        select.appendChild(option);
      }
      if (control.cameraDeviceId && !cameras.some((camera) => camera.id === control.cameraDeviceId)) {
        const remembered = document.createElement("option");
        remembered.value = control.cameraDeviceId;
        remembered.textContent = "上次设备（当前未发现）";
        select.appendChild(remembered);
      }
      select.value = control.cameraDeviceId || "";
      select.title = "选择该人员客户端上的摄像头设备";
      select.addEventListener("change", () => runAction(() => setCameraDesired(connection, control.enabled, select.value || null)));
      controls.appendChild(select);

      const toggle = document.createElement("button");
      toggle.className = control.enabled ? "danger" : "secondary";
      toggle.textContent = control.enabled ? "关闭摄像头" : "开启摄像头";
      toggle.addEventListener("click", () => runAction(() => setCameraDesired(connection, !control.enabled, select.value || null)));
      controls.appendChild(toggle);

      if (!control.online) {
        const repair = document.createElement("button");
        repair.className = "secondary";
        repair.textContent = "重新配对";
        repair.addEventListener("click", () => runAction(() => pairCamera(connection)));
        controls.appendChild(repair);
      }
      container.appendChild(controls);

      const details = document.createElement("span");
      details.className = "camera-details";
      details.textContent = cameraStatus(control);
      container.appendChild(details);
    }

    function cameraStatus(control) {
      if (control.error) return control.error;
      if (!control.online) return control.enabled ? "摄像头已启用，客户端重连后恢复" : "摄像头已关闭";
      if (!(control.cameras || []).length) return "未检测到摄像头";
      if (!control.capturing) return control.enabled ? "已下发开启，等待客户端" : "摄像头已关闭";
      const size = control.width && control.height ? control.width + "×" + control.height : "分辨率未知";
      const fps = control.fps ? " / " + Number(control.fps).toFixed(1) + " FPS" : "";
      const bitrate = control.bitrate ? " / " + (control.bitrate / 1000000).toFixed(1) + " Mbps" : "";
      return (control.codec || "H.264") + " / " + size + fps + bitrate + (control.cameraName ? " / " + control.cameraName : "");
    }

    async function pairCamera(connection) {
      const groupPassword = window.prompt("请输入该 SonoBus 群组密码；没有密码请留空：", "");
      if (groupPassword === null) return;
      const session = captureAdminSession();
      if (!adminSessionCurrent(session)) return;
      const result = await apiPost("/admin/video/pair", { group: connection.group, user: connection.user, groupPassword });
      if (!adminSessionCurrent(session)) return;
      const pairingText = "SBPAIR1." + result.pairingId + "." + result.pairingCode;
      try {
        await navigator.clipboard.writeText(pairingText);
        if (!adminSessionCurrent(session)) return;
        window.prompt("配对信息已复制。请在该人员的 SonoBus 客户端输入一次：", pairingText);
      } catch {
        if (!adminSessionCurrent(session)) return;
        window.prompt("请复制并在该人员的 SonoBus 客户端输入一次：", pairingText);
      }
      await refreshConnections();
      if (!adminSessionCurrent(session)) return;
      setStatus(connectionStatus, "已生成一次性摄像头配对信息；重新配对会立即撤销旧密钥并关闭旧连接。", false, true);
    }

    async function setCameraDesired(connection, enabled, cameraDeviceId) {
      const session = captureAdminSession();
      if (!adminSessionCurrent(session)) return;
      await apiPost("/admin/video/control", { group: connection.group, user: connection.user, enabled, cameraDeviceId });
      if (!adminSessionCurrent(session)) return;
      await refreshConnections();
      if (!adminSessionCurrent(session)) return;
      setStatus(connectionStatus, enabled ? "已下发开启命令；同群组其他摄像头已关闭。" : "已下发关闭命令。", false, true);
    }

    function renderBans(bans) {
      if (!bans.length) {
        bansBody.innerHTML = '<tr><td colspan="6" class="muted">当前没有封禁。</td></tr>';
        return;
      }
      bansBody.innerHTML = "";
      for (const ban of bans) {
        const tr = document.createElement("tr");
        const room = ban.group || ban.roomId || "-";
        const user = ban.user || ban.userId || "-";
        const address = ban.address || "-";
        tr.innerHTML =
          cell("类型", displayConnectionType(ban), ban.type) +
          cell("房间/群组", room, room) +
          cell("用户", user, user) +
          cell("IP", address, address) +
          cell("到期时间", displayExpiresAt(ban.expiresAt), ban.expiresAt || "永久") +
          '<td data-label="操作"><div class="actions"></div></td>';
        const actions = tr.querySelector(".actions");
        const unbanButton = document.createElement("button");
        unbanButton.className = "secondary";
        unbanButton.textContent = "解除";
        unbanButton.title = "解除这条封禁";
        unbanButton.addEventListener("click", () => runAction(() => unban(ban)));
        actions.appendChild(unbanButton);
        bansBody.appendChild(tr);
      }
    }

    async function unban(ban) {
      if (!confirm("确定解除 " + displayBan(ban) + " 的封禁吗？")) return;
      const session = captureAdminSession();
      if (!adminSessionCurrent(session)) return;
      const result = await apiPost("/admin/bans/remove", { id: ban.id });
      if (!adminSessionCurrent(session)) return;
      await refreshBans();
      if (!adminSessionCurrent(session)) return;
      setStatus(banStatus, "已解除 " + (result.removed || 0) + " 条封禁。", false, true);
    }

    async function runAction(action) {
      const generation = authGeneration;
      try {
        setStatus(connectionStatus, "正在操作...");
        await action();
      } catch (error) {
        if (authGeneration === generation) setStatus(connectionStatus, error.message, true);
      }
    }

    function streamRoom(connection) {
      const room = connection.videoRoom || connection.group || connection.roomId;
      if (!room) return null;
      return room.startsWith("SB_") ? room : "SB_" + room;
    }

    function videoUrl(connection) {
      const room = streamRoom(connection);
      if (!room) return null;
      return new URL("/" + encodeURIComponent(room), currentVideoUrls.browserBase || location.origin);
    }

    function openVideo(connection) {
      const url = videoUrl(connection);
      if (!url) {
        setStatus(connectionStatus, "该连接没有视频房间。", true);
        return;
      }
      window.open(url.toString(), "_blank", "noopener");
    }

    function obsUrl(connection) {
      const room = streamRoom(connection);
      if (!room) return null;
      const base = currentVideoUrls.rtspBase || ("rtsp://" + location.hostname + ":19092");
      return base.replace(/\/+$/, "") + "/" + encodeURIComponent(room);
    }

    async function copyObsUrl(connection) {
      const text = obsUrl(connection);
      if (!text) {
        setStatus(connectionStatus, "该连接没有视频房间。", true);
        return;
      }
      try {
        await navigator.clipboard.writeText(text);
        setStatus(connectionStatus, "已复制 OBS 媒体源 RTSP 地址：" + text, false, true);
      } catch {
        window.prompt("复制下面地址，并在 OBS 中添加“媒体源”：", text);
        setStatus(connectionStatus, "请在弹出的窗口中复制 OBS 媒体源地址。");
      }
    }

    function connectionPayload(connection) {
      if (connection.type === "sonobus-connection") {
        return { type: "sonobus-connection", group: connection.group, user: connection.user, address: connection.address, hasRelay: connection.hasRelay };
      }
      if (connection.type === "sonobus-udp") {
        return { type: "sonobus-udp", group: connection.group, user: connection.user };
      }
      if (connection.type === "udp-session") {
        return { type: "udp-session", sessionId: connection.sessionId };
      }
      return { type: "websocket", roomId: connection.roomId, userId: connection.userId };
    }

    async function apiGet(path) {
      const response = await fetch(apiUrl(path), { headers: authHeaders() });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "请求失败");
      return data;
    }

    async function apiPost(path, payload) {
      const response = await fetch(apiUrl(path), {
        method: "POST",
        headers: { ...authHeaders(), "content-type": "application/json" },
        body: JSON.stringify(payload)
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "请求失败");
      return data;
    }

    function authHeaders() {
      const token = localStorage.getItem(tokenKey);
      return token ? { authorization: "Bearer " + token } : {};
    }

    function apiUrl(path) {
      return baseUrlInput.value.replace(/\/+$/, "") + path;
    }

    function displayUser(connection) {
      return connection.user || connection.username || connection.userId || "该连接";
    }

    function displayBan(ban) {
      return ban.user || ban.userId || ban.address || ban.group || "该记录";
    }

    function displayExpiresAt(expiresAt) {
      return expiresAt ? new Date(expiresAt).toLocaleString() : "永久";
    }

    function isSystemBridge(connection) {
      return (connection.type === "sonobus-connection" || connection.type === "sonobus-udp")
        && (connection.user === "web-bridge" || String(connection.user || "").startsWith("media-mix-"));
    }

    function displayConnectionType(connection) {
      if (isSystemBridge(connection)) return "系统桥接服务";
      const labels = {
        "websocket": "桌面端 WebSocket",
        "udp-session": "桌面端 UDP 中继",
        "sonobus-udp": "SonoBus 音频中继",
        "sonobus-connection": "SonoBus 房间连接",
        "video-publisher": "SonoBus 视频发送",
        "video-viewer": "视频网页观看"
      };
      const type = typeof connection === "string" ? connection : connection.type;
      if (type === "sonobus-connection" && connection.hasRelay) {
        return "SonoBus 房间连接 + 音频中继";
      }
      return labels[type] || type || "-";
    }

    function displayRelayStats(connection) {
      if (connection.type !== "sonobus-udp" && !connection.hasRelay) return "-";
      return "收 " + (connection.packetsReceived || 0)
        + " / 转 " + (connection.packetsForwarded || 0)
        + "\n末包 " + (connection.lastPacketBytes || 0) + "B"
        + " / 末转 " + (connection.lastForwardCount || 0);
    }

    function displayRelayDiagnostic(diagnostic) {
      if (!diagnostic) return "";
      const parts = [];
      if (diagnostic.invalidSonoBusPackets) {
        parts.push("无效 SBR1 中继包 " + diagnostic.invalidSonoBusPackets
          + " 个，最近来自 " + (diagnostic.lastInvalidSonoBusPacketFrom || "-")
          + "，原因：" + (diagnostic.lastInvalidSonoBusPacketReason || "-"));
      }
      if (diagnostic.unknownUdpPackets) {
        parts.push("未知 UDP 包 " + diagnostic.unknownUdpPackets
          + " 个，最近来自 " + (diagnostic.lastUnknownUdpPacketFrom || "-"));
      }
      return parts.length ? "；" + parts.join("；") : "";
    }

    function memberCell(room, user, systemBridge) {
      const systemBadge = systemBridge ? '<span class="state-badge neutral">系统服务</span>' : "";
      return '<td data-label="成员" title="' + escapeHtml(user + " / " + room) + '"><div class="member-meta">'
        + '<strong>' + escapeHtml(user) + '</strong>'
        + '<span class="meta-line">' + escapeHtml(room) + '</span>'
        + systemBadge + '</div></td>';
    }

    function connectionCell(connection, address, port, lastSeen, online) {
      const endpoint = address + (String(port) === "-" ? "" : ":" + port);
      const seen = lastSeen === "-" ? "-" : new Date(lastSeen).toLocaleString();
      return '<td data-label="连接"><div class="connection-meta">'
        + '<div class="connection-head"><span class="state-badge ' + (online ? "" : "offline") + '">'
        + (online ? "在线" : "离线") + '</span><strong>' + escapeHtml(displayConnectionType(connection)) + '</strong></div>'
        + '<span class="meta-line" title="' + escapeHtml(endpoint) + '">' + escapeHtml(endpoint) + '</span>'
        + '<span class="meta-line">最近活跃 ' + escapeHtml(seen) + '</span></div></td>';
    }


    function cell(label, text, title = "") {
      return '<td data-label="' + escapeHtml(label) + '" title="' + escapeHtml(title || text || "-") + '">' + escapeHtml(text || "-") + '</td>';
    }

    function relayCell(text) {
      const escaped = escapeHtml(text || "-");
      return '<td class="relay-cell" data-label="中继包" title="' + escaped.replace(/\n/g, " ") + '"><div class="relay-stats">'
        + escaped.split("\n").map((line) => "<span>" + line + "</span>").join("")
        + "</div></td>";
    }

    function escapeHtml(value) {
      return String(value).replace(/[&<>"']/g, (char) => ({
        "&": "&amp;",
        "<": "&lt;",
        ">": "&gt;",
        '"': "&quot;",
        "'": "&#39;"
      })[char]);
    }

    function setStatus(element, text, isError = false, isOk = false) {
      element.textContent = text;
      element.classList.toggle("error", isError);
      element.classList.toggle("ok", isOk && !isError);
    }
  </script>
</body>
</html>`;
