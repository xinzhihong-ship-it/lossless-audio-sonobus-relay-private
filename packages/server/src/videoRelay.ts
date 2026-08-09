import dgram from "node:dgram";
import { randomUUID } from "node:crypto";
import type { IncomingMessage } from "node:http";
import WebSocket from "ws";
import { VideoPresenceRegistry } from "./videoPresence.js";

type VideoSocket = {
  room: string;
  user: string;
  sessionId: string;
  socket: WebSocket;
  timer: NodeJS.Timeout;
};

type FrameAssembly = {
  room: string;
  user: string;
  frameId: number;
  chunkCount: number;
  chunks: Map<number, Buffer>;
  bytes: number;
  createdAt: number;
};

const MEDIA_MAGIC = "SBV1";
const MEDIA_HEADER_BYTES = 14;
const MAX_MEDIA_CHUNKS = 512;
const MAX_MEDIA_FRAME_BYTES = 2 * 1024 * 1024;
const MEDIA_ASSEMBLY_TTL_MS = 2_000;
const MAX_VIEWER_BUFFERED_BYTES = 128 * 1024;

export class VideoRelayHub {
  private readonly publishers = new Map<string, VideoSocket>();
  private readonly viewers = new Map<string, VideoSocket>();
  private readonly assemblies = new Map<string, FrameAssembly>();
  private mediaSocket?: dgram.Socket;

  constructor(
    private readonly presence: VideoPresenceRegistry,
    private readonly mediaPort?: number
  ) {}

  async start(): Promise<void> {
    if (this.mediaPort === undefined) {
      return;
    }

    const socket = dgram.createSocket("udp4");
    socket.on("message", (message) => this.handleMediaMessage(message));
    socket.on("error", () => {
      // The HTTP/WebSocket relay remains usable if a media datagram is malformed.
    });
    await new Promise<void>((resolve, reject) => {
      const onError = (error: Error) => {
        socket.off("listening", onListening);
        reject(error);
      };
      const onListening = () => {
        socket.off("error", onError);
        resolve();
      };
      socket.once("error", onError);
      socket.once("listening", onListening);
      socket.bind(this.mediaPort, "0.0.0.0");
    });
    this.mediaSocket = socket;
  }

  joinPublisher(room: string, user: string, camera: string | undefined, req: IncomingMessage, socket: WebSocket): void {
    this.join(this.publishers, "video-publisher", room, user, camera, req, socket, (frame) => {
      this.broadcastFrame(room, frame);
    });
  }

  joinViewer(room: string, user: string, req: IncomingMessage, socket: WebSocket): void {
    this.join(this.viewers, "video-viewer", room, user, undefined, req, socket);
  }

  async close(): Promise<void> {
    for (const connection of [...this.publishers.values(), ...this.viewers.values()]) {
      clearInterval(connection.timer);
      connection.socket.close(1001, "Video relay is shutting down.");
    }
    this.publishers.clear();
    this.viewers.clear();
    this.assemblies.clear();

    if (this.mediaSocket) {
      const socket = this.mediaSocket;
      this.mediaSocket = undefined;
      await new Promise<void>((resolve) => socket.close(() => resolve()));
    }
  }

  private join(
    collection: Map<string, VideoSocket>,
    type: "video-publisher" | "video-viewer",
    room: string,
    user: string,
    camera: string | undefined,
    req: IncomingMessage,
    socket: WebSocket,
    onFrame?: (frame: Buffer) => void
  ): void {
    const sessionId = randomUUID();
    const key = `${room}\u0000${user}\u0000${type}`;
    const previous = collection.get(key);
    if (previous) {
      clearInterval(previous.timer);
      previous.socket.close(4001, "Video connection replaced.");
    }

    const heartbeat = () => {
      this.presence.heartbeat({
        sessionId,
        type,
        group: room,
        user,
        camera,
        address: req.socket.remoteAddress ?? undefined,
        port: req.socket.remotePort
      });
    };
    heartbeat();

    const connection: VideoSocket = {
      room,
      user,
      sessionId,
      socket,
      timer: setInterval(heartbeat, 5_000)
    };
    collection.set(key, connection);

    socket.on("message", (data, isBinary) => {
      heartbeat();
      if (!isBinary || !onFrame) {
        return;
      }
      onFrame(toBuffer(data));
    });
    socket.on("close", () => {
      clearInterval(connection.timer);
      if (collection.get(key)?.sessionId === sessionId) {
        collection.delete(key);
      }
      this.presence.remove(sessionId);
      for (const [assemblyKey, assembly] of this.assemblies) {
        if (assembly.room === room && assembly.user === user) {
          this.assemblies.delete(assemblyKey);
        }
      }
    });
    socket.on("error", () => socket.close());
  }

  private handleMediaMessage(message: Buffer): void {
    this.purgeAssemblies();
    if (message.length < MEDIA_HEADER_BYTES || message.toString("ascii", 0, 4) !== MEDIA_MAGIC) {
      return;
    }

    const roomBytes = message[4];
    const userBytes = message[5];
    const frameId = message.readUInt32BE(6);
    const chunkIndex = message.readUInt16BE(10);
    const chunkCount = message.readUInt16BE(12);
    const metadataBytes = MEDIA_HEADER_BYTES + roomBytes + userBytes;
    if (roomBytes === 0 || userBytes === 0 || chunkCount === 0 || chunkCount > MAX_MEDIA_CHUNKS
      || chunkIndex >= chunkCount || metadataBytes >= message.length) {
      return;
    }

    const room = message.toString("utf8", MEDIA_HEADER_BYTES, MEDIA_HEADER_BYTES + roomBytes);
    const user = message.toString("utf8", MEDIA_HEADER_BYTES + roomBytes, metadataBytes);
    const key = `${room}\u0000${user}\u0000video-publisher`;
    const publisher = this.publishers.get(key);
    if (!publisher || publisher.socket.readyState !== WebSocket.OPEN) {
      return;
    }

    const assemblyKey = `${publisher.sessionId}\u0000${frameId}`;
    let assembly = this.assemblies.get(assemblyKey);
    if (!assembly || assembly.chunkCount !== chunkCount) {
      assembly = {
        room,
        user,
        frameId,
        chunkCount,
        chunks: new Map(),
        bytes: 0,
        createdAt: Date.now()
      };
      this.assemblies.set(assemblyKey, assembly);
    }

    if (!assembly.chunks.has(chunkIndex)) {
      const payload = message.subarray(metadataBytes);
      assembly.bytes += payload.length;
      if (assembly.bytes > MAX_MEDIA_FRAME_BYTES) {
        this.assemblies.delete(assemblyKey);
        return;
      }
      assembly.chunks.set(chunkIndex, Buffer.from(payload));
    }

    if (assembly.chunks.size !== assembly.chunkCount) {
      return;
    }

    const chunks: Buffer[] = [];
    for (let index = 0; index < assembly.chunkCount; ++index) {
      const chunk = assembly.chunks.get(index);
      if (!chunk) {
        this.assemblies.delete(assemblyKey);
        return;
      }
      chunks.push(chunk);
    }
    const frame = Buffer.concat(chunks);
    this.assemblies.delete(assemblyKey);
    this.broadcastFrame(room, frame);
  }

  private broadcastFrame(room: string, frame: Buffer): void {
    for (const viewer of this.viewers.values()) {
      if (viewer.room !== room || viewer.socket.readyState !== WebSocket.OPEN
        || viewer.socket.bufferedAmount > MAX_VIEWER_BUFFERED_BYTES) {
        continue;
      }
      viewer.socket.send(frame, { binary: true });
    }
  }

  private purgeAssemblies(): void {
    const cutoff = Date.now() - MEDIA_ASSEMBLY_TTL_MS;
    for (const [key, assembly] of this.assemblies) {
      if (assembly.createdAt < cutoff) {
        this.assemblies.delete(key);
      }
    }
    while (this.assemblies.size > 1024) {
      const first = this.assemblies.keys().next().value;
      if (first === undefined) {
        break;
      }
      this.assemblies.delete(first);
    }
  }
}

function toBuffer(data: WebSocket.RawData): Buffer {
  if (Buffer.isBuffer(data)) {
    return data;
  }
  if (data instanceof ArrayBuffer) {
    return Buffer.from(data);
  }
  return Buffer.concat(data);
}
