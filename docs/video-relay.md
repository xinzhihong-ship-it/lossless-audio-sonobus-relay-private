# 自建 H.264 / WebRTC 视频中转

视频媒体面完全自建：SonoBus 只轮询管理员控制状态；macOS 由 FFmpeg 采集，Windows 由独立 `SonoBusVideoCaptureHelper.exe` 以 `SharedReadOnly` 读取摄像头，再交给 FFmpeg 编码和 RTSP 发布。MediaMTX 负责 WebRTC/WHEP、RTSP 和观看者扇出。helper 崩溃或摄像头/网络中断不会进入音频回调，也不应拖垮 DAW。

公共流同时包含：

- 摄像头低延迟 H.264：`yuv420p`、无 B 帧、GOP 不超过 1 秒；优先 VideoToolbox/NVENC/QSV/AMF/Media Foundation。
- 当前 SonoBus 群组的完整混音：Opus 48 kHz、立体声、160 kbps。

SonoBus 原有音频 Relay 仍使用 UDP `9000`，协议和行为不变。

## 端口

- `19090/TCP`：公开观看入口、WebRTC/WHEP HTTP、健康检查；不公开管理员页面。
- `19091/UDP`：MediaMTX WebRTC ICE 媒体。
- `19092/TCP`：摄像头/服务端合流 RTSP 发布和 OBS RTSP 读取。
- `19094/TCP`：只绑定服务器 `127.0.0.1` 的管理员页面；必须通过 SSH 隧道访问。
- `9000/UDP`：SonoBus 音频 Relay。
- `10998/TCP+UDP`：SonoBus Connection Server。

当前部署不依赖公网 `80/443`、域名或 HTTPS。

## 管理员摄像头控制

1. 用户在 SonoBus 加入群组；客户端通过该已认证会话自动发起待授权请求，不显示或传递授权码。
2. 管理员通过 SSH 隧道打开 `http://127.0.0.1:19094/admin`，确认在线用户并点击“授权摄像头”。
3. 客户端自动领取并验证批准结果，将 32 字节密钥保存到 macOS 钥匙串或 Windows 凭据管理器；DAW 工程只保存配对 ID。
4. 用户只需在操作系统首次询问时允许本机摄像头访问。之后只有管理员页面可以开启/关闭摄像头和选择设备；客户端只显示状态并可撤销本机授权。
5. 每个群组最多一路摄像头；开启另一名成员时，上一名成员自动关闭，公共路径保持不变。
6. 管理员的开关和设备选择持久化；重新连接或重启后恢复。

管理员页面显示音频在线、视频控制在线、实际采集、codec、分辨率、FPS、bitrate、MediaMTX publisher/viewer 和动态音频合流状态。系统账号 `web-bridge` 不提供配对、开关、踢出或封禁等用户操作。

## 发送端

- 客户端从现有 SonoBus 服务器地址派生 `19090` 控制地址和 `19092` RTSP 地址，不增加用户可见的视频服务器输入项。
- 控制轮询使用 HMAC-SHA256、时间戳、单调序列号和 nonce 防伪造、防重放。
- Windows 使用 `MediaCaptureSharingMode::SharedReadOnly` 和 `MediaFrameReader`，不会主动取得摄像头独占权；FFmpeg 只接收 NV12 帧、编码 H.264 并发布 RTSP。
- `SharedReadOnly` 禁止修改摄像头格式。Windows 在正式发布 helper 中只打开一次当前彩色源，选择当下可共享的最高分辨率×FPS源；后台上限只作用于 FFmpeg 输出，自动按实际源分辨率/FPS计算码率，超出源能力时自动钳制。降分辨率只下采样，降 FPS 只丢帧，绝不放大或补帧。
- 不再先用独立 helper 进程探测模式再重复打开摄像头；管理员选定的设备被独占、断开或共享模式失效时，客户端不会静默切换其他摄像头，并按最长 30 秒退避重试，同时保留 helper 的结构化错误。
- macOS 先读取设备实际模式，再按像素面积从高到低验证 60 FPS，绝不静默固定为 `640×360`。
- Windows/macOS 安装包携带固定、校验过的 FFmpeg；详见 [`ffmpeg-runtime.md`](ffmpeg-runtime.md)。
- helper 和网络重连均在非音频线程/独立进程完成。

## 浏览器与 OBS

群组 `studio` 的稳定公共路径为 `SB_studio`：

```text
浏览器：         http://<服务器>:19090/SB_studio
WHEP：           http://<服务器>:19090/SB_studio/whep
OBS Media Source：rtsp://<服务器>:19092/SB_studio
```

OBS 必须添加 **Media Source（媒体源）**，而不是旧的浏览器 JPEG 源；关闭“Local File”，粘贴管理后台复制的 RTSP URL。视频和群组 Opus 音频都来自同一稳定 RTSP 路径。

## 部署

从 `deploy/.env.example` 创建 `deploy/.env`，至少更换：

- `JWT_SECRET`
- `ADMIN_PASSWORD`
- `POSTGRES_PASSWORD`
- `MEDIA_MTX_API_PASSWORD`
- `MEDIA_MUXER_PASSWORD`
- `CONNECTION_SERVER_ADMIN_TOKEN`

云安全组只放行 TCP/UDP `10998`、UDP `9000`、TCP `19090`、UDP `19091`、TCP `19092`。不要放行 `19094`；管理员使用：

```bash
ssh -L 19094:127.0.0.1:19094 ubuntu@<服务器>
```

然后本机访问 `http://127.0.0.1:19094/admin`。
