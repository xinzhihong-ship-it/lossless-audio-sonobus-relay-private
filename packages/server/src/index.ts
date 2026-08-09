import { createApp, type ServerConfig } from "./app.js";

const mediaMuxerUsername = process.env.MEDIA_MUXER_USERNAME ?? "media-muxer";
const mediaMuxerPassword = required("MEDIA_MUXER_PASSWORD", process.env.MEDIA_MUXER_PASSWORD);
const mediaMtxApiUsername = process.env.MEDIA_MTX_API_USERNAME ?? "media-api";
const mediaMtxApiPassword = required("MEDIA_MTX_API_PASSWORD", process.env.MEDIA_MTX_API_PASSWORD);
const connectionServerAdminUrl = process.env.CONNECTION_SERVER_ADMIN_URL;

const config: ServerConfig = {
  jwtSecret: required("JWT_SECRET", process.env.JWT_SECRET),
  adminUsername: process.env.ADMIN_USERNAME ?? "admin",
  adminPassword: process.env.ADMIN_PASSWORD ?? "admin123456",
  databaseUrl: process.env.DATABASE_URL,
  maxBytesPerSecondPerClient: Number(process.env.MAX_BYTES_PER_SECOND_PER_CLIENT ?? 50 * 1024 * 1024),
  udpRelayPort: Number(process.env.UDP_RELAY_PORT ?? 9000),
  connectionServerAdminUrl,
  connectionServerAdminToken: connectionServerAdminUrl
    ? required("CONNECTION_SERVER_ADMIN_TOKEN", process.env.CONNECTION_SERVER_ADMIN_TOKEN)
    : undefined,
  webBridgeAdminUrl: process.env.WEB_BRIDGE_ADMIN_URL,
  publicVideoHost: required("PUBLIC_VIDEO_HOST", process.env.PUBLIC_VIDEO_HOST),
  publicHttpPort: Number(process.env.PUBLIC_HTTP_PORT ?? 19090),
  mediaMtxAdminUrl: process.env.MEDIA_MTX_ADMIN_URL,
  mediaMtxApiUsername,
  mediaMtxApiPassword,
  videoRtspPort: Number(process.env.VIDEO_RTSP_PORT ?? 19092),
  mediaMuxerUsername,
  mediaMuxerPassword,
  groupMediaManagerConfig: process.env.VIDEO_BRIDGE_BINARY && process.env.FFMPEG_BINARY
    ? {
        bridgeBinary: process.env.VIDEO_BRIDGE_BINARY,
        ffmpegBinary: process.env.FFMPEG_BINARY,
        connectionHost: process.env.VIDEO_BRIDGE_CONNECTION_HOST ?? "connection-server",
        connectionPort: Number(process.env.CONNECTION_SERVER_PORT ?? 10998),
        relayHost: process.env.VIDEO_BRIDGE_RELAY_HOST ?? "server",
        relayPort: Number(process.env.UDP_RELAY_PORT ?? 9000),
        mediaMtxRtspHost: process.env.MEDIA_MTX_RTSP_HOST ?? "mediamtx",
        mediaMtxRtspPort: Number(process.env.MEDIA_MTX_RTSP_PORT ?? 8554),
        muxerUsername: mediaMuxerUsername,
        muxerPassword: mediaMuxerPassword
      }
    : undefined
};

const port = Number(process.env.PORT ?? 8080);
const app = await createApp(config);
app.server.listen(port, "0.0.0.0", () => {
  console.log(`lossless audio relay listening on :${port}`);
});

process.on("SIGTERM", () => {
  app.close().finally(() => process.exit(0));
});

function required(name: string, value: string | undefined): string {
  if (!value) {
    throw new Error(`${name} is required.`);
  }
  return value;
}
