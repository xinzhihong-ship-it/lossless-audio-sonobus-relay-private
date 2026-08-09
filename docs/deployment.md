# Ubuntu 公网服务器部署

本部署不要求域名、备案、公网 `80/443` 或 HTTPS。示例公网入口为 `http://<服务器IP>:19090`；管理员页面只绑定服务器 loopback，必须走 SSH 隧道。

## 服务与端口

Docker Compose 运行：

| 服务 | 作用 |
| --- | --- |
| `postgres` | 用户、封禁和摄像头控制持久化 |
| `mediamtx` | H.264 RTSP、WebRTC/WHEP、浏览器/OBS 扇出 |
| `server` | Node 管理、认证、UDP 音频 Relay、动态群组音频合流 |
| `connection-server` | SonoBus group 发现与管理 |
| `web-bridge` | 固定 Web 音频桥；系统账号 `web-bridge` |
| `caddy` | 公网观看入口与 loopback 管理入口 |

公网安全组只放行：

| 端口 | 协议 | 用途 |
| --- | --- | --- |
| `10998` | TCP + UDP | SonoBus Connection Server |
| `9000` | UDP | SonoBus 音频 Relay |
| `19090` | TCP | 浏览器观看、Web 音频、WebRTC/WHEP HTTP、健康检查 |
| `19091` | UDP | MediaMTX WebRTC ICE 媒体 |
| `19092` | TCP | RTSP 发布、OBS Media Source 读取 |

`19094/TCP` 只绑定服务器 `127.0.0.1`，**不要**加入公网安全组。MediaMTX Control API `9997` 也不映射到主机。

## 1. 获取并上传部署包

在私有仓库 GitHub Actions 下载最新成功的 `lossless-audio-server-linux-docker`，得到：

```text
lossless-audio-server-linux-docker.tar.gz
```

上传：

```bash
scp lossless-audio-server-linux-docker.tar.gz ubuntu@<服务器IP>:/home/ubuntu/
```

## 2. 安装或升级

首次安装：

```bash
sudo mkdir -p /opt/lossless-audio
sudo tar -xzf /home/ubuntu/lossless-audio-server-linux-docker.tar.gz \
  -C /opt/lossless-audio --strip-components=1
sudo chown -R ubuntu:ubuntu /opt/lossless-audio
cd /opt/lossless-audio/deploy
cp .env.example .env
```

升级现有服务时保留数据库 volume 和现有 `.env`：

```bash
sudo cp /opt/lossless-audio/deploy/.env \
  /root/lossless-audio.env.backup.$(date +%Y%m%d-%H%M%S)
sudo tar -xzf /home/ubuntu/lossless-audio-server-linux-docker.tar.gz \
  -C /opt/lossless-audio --strip-components=1
sudo chown -R ubuntu:ubuntu /opt/lossless-audio
```

若旧 `.env` 缺少新变量，请对照 `.env.example` 补齐，尤其是 `PUBLIC_VIDEO_HOST`、`MEDIA_MTX_API_PASSWORD` 和 `MEDIA_MUXER_PASSWORD`。

## 3. 配置 `.env`

编辑：

```bash
cd /opt/lossless-audio/deploy
nano .env
```

至少确认：

```dotenv
PUBLIC_DOMAIN=:80
PUBLIC_HTTP_PORT=19090
ADMIN_HTTP_PORT=19094
PUBLIC_VIDEO_HOST=<服务器公网IP>
VIDEO_WEBRTC_PORT=19091
VIDEO_RTSP_PORT=19092

MEDIA_MTX_API_USERNAME=media-api
MEDIA_MTX_API_PASSWORD=<长随机 base64url 密码>
MEDIA_MUXER_USERNAME=media-muxer
MEDIA_MUXER_PASSWORD=<另一条长随机密码>

JWT_SECRET=<长随机密钥>
ADMIN_USERNAME=admin
ADMIN_PASSWORD=<管理员强密码>
POSTGRES_DB=lossless_audio
POSTGRES_USER=lossless_audio
POSTGRES_PASSWORD=<数据库强密码>

UDP_RELAY_PORT=9000
CONNECTION_SERVER_PORT=10998
WEB_BRIDGE_GROUP=web
WEB_BRIDGE_USERNAME=web-bridge
WEB_BRIDGE_GROUP_PASSWORD=
```

生成随机值：

```bash
openssl rand -hex 32       # JWT_SECRET
openssl rand -base64 36 | tr -d '\n'  # 其他密码
```

不要把真实 `.env`、配对信息或临时 SSH 私钥提交到 GitHub。

## 4. 启动

```bash
cd /opt/lossless-audio/deploy
docker compose config -q
docker compose up -d --build
docker compose ps
```

只更新 Node、MediaMTX 或 Caddy 时可避免重建 Connection Server/Web Bridge：

```bash
docker compose up -d --build server mediamtx caddy
```

## 5. 健康检查

```bash
curl -fsS http://127.0.0.1:19090/health
docker compose ps
docker compose logs --tail=100 server mediamtx caddy
sudo ss -lntup | grep -E ':(9000|10998|19090|19091|19092|19094)\b'
```

预期：

- `19090/TCP`、`19092/TCP` 对公网监听。
- `19091/UDP` 对公网监听。
- `19094/TCP` 只出现 `127.0.0.1:19094`。
- `9997` 不出现在主机监听列表。

公网验证：

```bash
curl -fsS http://<服务器IP>:19090/health
```

## 6. 打开管理员页面

在管理员电脑建立隧道：

```bash
ssh -L 19094:127.0.0.1:19094 ubuntu@<服务器IP>
```

保持 SSH 会话打开，再访问：

```text
http://127.0.0.1:19094/admin
```

公网 `http://<服务器IP>:19090/admin` 应返回 `404`，这是预期安全行为。

管理员页面可以：

- 查看 SonoBus 音频连接、视频控制会话和 MediaMTX 路径。
- 踢出/封禁普通用户；系统 `web-bridge` 不显示这些操作。
- 为当前在线的 group/user 生成一次性摄像头配对信息。
- 独占控制每组一路摄像头的开关和设备。
- 查看 codec、分辨率、FPS、bitrate、publisher/viewer 和音频合流状态。
- 打开浏览器观看地址，或复制 OBS RTSP 地址。

## 7. 客户端与观看

SonoBus 客户端保持原有服务器填写方式：

```text
Connection Server: <服务器IP>
Connection Port:   10998
Use Relay:          开启
Relay Server:       <服务器IP>
Relay Port:         9000
```

客户端不填写视频端口。首次配对流程见 [`video-relay.md`](video-relay.md)。

群组 `studio`：

```text
浏览器：          http://<服务器IP>:19090/SB_studio
OBS Media Source：rtsp://<服务器IP>:19092/SB_studio
```

OBS 添加 **Media Source（媒体源）**，关闭“Local File”，不要使用旧浏览器源。

## 8. 数据、备份与回滚

数据库位于 Docker volume `postgres-data`。备份：

```bash
cd /opt/lossless-audio/deploy
docker compose exec -T postgres pg_dump -U "${POSTGRES_USER:-lossless_audio}" lossless_audio \
  > /root/lossless-audio-$(date +%Y%m%d-%H%M%S).sql
```

部署前也可完整备份目录：

```bash
sudo cp -a /opt/lossless-audio \
  /opt/lossless-audio.backup-$(date +%Y%m%d-%H%M%S)
```

回滚时恢复旧目录和对应 `.env`，再执行 `docker compose up -d --build`；不要删除数据库 volume。

## 9. 常见问题

### 浏览器页面可开但无视频

检查 UDP `19091` 安全组、`PUBLIC_VIDEO_HOST` 是否为公网 IP，以及：

```bash
docker compose logs --tail=200 mediamtx server
```

### OBS 无法播放

确认使用 `rtsp://<服务器IP>:19092/SB_<群组>`、TCP `19092` 已放行，且管理员页面显示公共路径 ready。

### 摄像头不启动

确认用户仍在对应 SonoBus group、配对未撤销、管理员已开启设备。客户端应显示 helper 的模式探测或错误；摄像头/网络恢复后无需重启 DAW。

### 动态群组无音频

重新配对时填写完全相同的 SonoBus group 密码（包括空格）；查看管理页音频桥状态及 `server` 日志。

### 磁盘不足

```bash
df -h
docker builder prune -af
```

只清理构建缓存；不要删除 `postgres-data`。
