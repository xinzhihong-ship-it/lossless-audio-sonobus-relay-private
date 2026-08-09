# FFmpeg camera runtime provenance

SonoBus ships FFmpeg only as a crash-isolated camera/H.264 companion process. It is not linked into the SonoBus binaries.

## macOS

- FFmpeg: `7.1.1`
- Source: <https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz>
- Source SHA-256: `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`
- Build recipe: [`tools/macos/build-ffmpeg-video.sh`](../tools/macos/build-ffmpeg-video.sh)
- Architectures: `x86_64`, `arm64`
- Enabled camera/codec path: AVFoundation, VideoToolbox H.264, RTSP/TCP
- License: LGPL-2.1-or-later; the package includes `ffmpeg-LICENSE-LGPL-2.1`.

The source archive and complete reproducible configuration are available from the URL and build recipe above.

## Windows

- Distribution: Gyan `7.1.1 essentials_build`
- Archive: <https://github.com/GyanD/codexffmpeg/releases/download/7.1.1/ffmpeg-7.1.1-essentials_build.zip>
- Archive SHA-256: `04861d3339c5ebe38b56c19a15cf2c0cc97f5de4fa8910e4d47e5e6404e4a2d4`
- FFmpeg source revision reported by the distributor: <https://github.com/FFmpeg/FFmpeg/commit/db69d06eee>
- Capture boundary: `SonoBusVideoCaptureHelper.exe` uses Windows `MediaCapture` in `SharedReadOnly` mode and writes NV12 to FFmpeg stdin; FFmpeg never opens DirectShow or owns the camera.
- Required FFmpeg path: rawvideo/NV12 input, H.264 encoder, and RTSP/TCP muxer.
- License: GPL-3.0; the unmodified distributor `LICENSE` and `README.txt` are included with every Windows package.

The Windows runtime is distributed alongside GPLv3 SonoBus. The included distributor README records its build configuration, external libraries, versions, and corresponding source revision.
