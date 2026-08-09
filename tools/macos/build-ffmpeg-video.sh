#!/usr/bin/env bash
set -euo pipefail

FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-${ROOT}/artifacts/ffmpeg-macos-universal}"
WORK="${RUNNER_TEMP:-/tmp}/sonobus-ffmpeg-${FFMPEG_VERSION}"
ARCHIVE="${WORK}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
SOURCE="${WORK}/ffmpeg-${FFMPEG_VERSION}"
JOBS="$(sysctl -n hw.logicalcpu)"

rm -rf "${WORK}" "${OUT}"
mkdir -p "${WORK}" "${OUT}"
curl --fail --location --silent --show-error --retry 4 --retry-all-errors \
  "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
  --output "${ARCHIVE}"
echo "${FFMPEG_SHA256}  ${ARCHIVE}" | shasum -a 256 --check
tar -xf "${ARCHIVE}" -C "${WORK}"

for arch in x86_64 arm64; do
  build="${WORK}/build-${arch}"
  prefix="${WORK}/install-${arch}"
  mkdir -p "${build}" "${prefix}"
  (
    cd "${build}"
    "${SOURCE}/configure" \
      --prefix="${prefix}" \
      --arch="${arch}" \
      --target-os=darwin \
      --cc=clang \
      --enable-cross-compile \
      --disable-autodetect \
      --disable-everything \
      --disable-doc \
      --disable-debug \
      --disable-x86asm \
      --disable-ffplay \
      --disable-ffprobe \
      --enable-ffmpeg \
      --enable-avdevice \
      --enable-avfilter \
      --enable-swscale \
      --enable-network \
      --enable-avfoundation \
      --enable-videotoolbox \
      --enable-indev=avfoundation \
      --enable-demuxer=rawvideo \
      --enable-decoder=rawvideo \
      --enable-encoder=h264_videotoolbox \
      --enable-muxer=rtsp \
      --enable-muxer=rtp \
      --enable-muxer=null \
      --enable-protocol=file \
      --enable-protocol=pipe \
      --enable-protocol=tcp \
      --enable-protocol=udp \
      --enable-filter=format \
      --enable-filter=scale \
      --extra-cflags="-arch ${arch} -mmacosx-version-min=10.13" \
      --extra-ldflags="-arch ${arch} -mmacosx-version-min=10.13"
    make -j"${JOBS}" ffmpeg
    cp ffmpeg "${prefix}/ffmpeg"
  )
done

lipo -create \
  "${WORK}/install-x86_64/ffmpeg" \
  "${WORK}/install-arm64/ffmpeg" \
  -output "${OUT}/ffmpeg"
chmod 0755 "${OUT}/ffmpeg"
cp "${SOURCE}/COPYING.LGPLv2.1" "${OUT}/ffmpeg-LICENSE-LGPL-2.1"

architectures="$(lipo -archs "${OUT}/ffmpeg")"
[[ " ${architectures} " == *" x86_64 "* && " ${architectures} " == *" arm64 "* ]]
"${OUT}/ffmpeg" -hide_banner -encoders > "${WORK}/encoders.txt" 2>&1
grep -q h264_videotoolbox "${WORK}/encoders.txt"
"${OUT}/ffmpeg" -hide_banner -devices > "${WORK}/devices.txt" 2>&1
grep -q avfoundation "${WORK}/devices.txt"
"${OUT}/ffmpeg" -hide_banner -muxers > "${WORK}/muxers.txt" 2>&1
grep -q ' null ' "${WORK}/muxers.txt"
grep -q ' rtsp ' "${WORK}/muxers.txt"
shasum -a 256 "${OUT}/ffmpeg"
echo "Built FFmpeg ${FFMPEG_VERSION}: ${architectures}"
