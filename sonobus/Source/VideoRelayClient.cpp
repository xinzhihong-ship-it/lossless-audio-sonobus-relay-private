// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include "VideoRelayClient.h"
#include <cstring>

namespace
{
constexpr int videoPort = 19090;
constexpr int videoMediaPort = 19091;
constexpr size_t mediaChunkPayloadBytes = 1200;
constexpr size_t mediaHeaderBytes = 14;
constexpr int frameWidth = 640;
constexpr int frameHeight = 360;
constexpr int frameIntervalMs = 66;

juce::String makeRoomName(const juce::String& group)
{
    return group.startsWith("SB_") ? group : "SB_" + group;
}
}

VideoRelayClient::VideoRelayClient()
    : juce::Thread("SonoBus video relay")
{
}

VideoRelayClient::~VideoRelayClient()
{
    stop();
}

juce::StringArray VideoRelayClient::getCameraDevices()
{
#if SONOBUS_CAMERA_SUPPORTED
    return juce::CameraDevice::getAvailableDevices();
#else
    return {};
#endif
}

void VideoRelayClient::start(const juce::String& host_,
                             const juce::String& group_,
                             const juce::String& user_,
                             const juce::String& preferredCamera_)
{
    stop();

    {
        const juce::ScopedLock lock(configLock);
        host = host_.trim();
        group = group_.trim();
        user = user_.trim();
        preferredCamera = preferredCamera_.trim();
    }

    if (host.isEmpty() || group.isEmpty() || user.isEmpty())
    {
        setStatus(Status::error);
        return;
    }

    setStatus(Status::connecting);
    startThread();
}

void VideoRelayClient::stop()
{
    signalThreadShouldExit();
    socket.close();
    stopThread(2500);

    const juce::ScopedLock lock(frameLock);
    latestJpeg.reset();
    frameNumber = 0;
    activeCamera.clear();
    setStatus(Status::idle);
}

void VideoRelayClient::run()
{
#if !SONOBUS_CAMERA_SUPPORTED
    setStatus(Status::cameraUnavailable);
    while (! threadShouldExit())
        wait(500);
#else
    while (! threadShouldExit())
    {
        juce::String localPreferredCamera;
        {
            const juce::ScopedLock lock(configLock);
            localPreferredCamera = preferredCamera;
        }

        const auto devices = getCameraDevices();
        if (devices.isEmpty())
        {
            setStatus(Status::cameraUnavailable);
            for (int i = 0; i < 10 && ! threadShouldExit(); ++i)
                wait(500);
            continue;
        }

        int deviceIndex = 0;
        if (localPreferredCamera.isNotEmpty())
        {
            for (int i = 0; i < devices.size(); ++i)
            {
                if (devices[i] == localPreferredCamera)
                {
                    deviceIndex = i;
                    break;
                }
            }
        }

        camera.reset(juce::CameraDevice::openDevice(deviceIndex,
                                                     frameWidth,
                                                     frameHeight,
                                                     1280,
                                                     720,
                                                     true));
        if (camera == nullptr)
        {
            setStatus(Status::cameraUnavailable);
            for (int i = 0; i < 10 && ! threadShouldExit(); ++i)
                wait(500);
            continue;
        }

        {
            const juce::ScopedLock lock(frameLock);
            activeCamera = devices[deviceIndex];
        }
        camera->addListener(this);

        if (! connectWebSocket())
        {
            camera->removeListener(this);
            camera.reset();
            if (threadShouldExit())
                break;
            setStatus(Status::error);
            wait(1000);
            continue;
        }

        setStatus(Status::online);
        juce::uint32 sentFrameNumber = 0;
        auto lastFrameAt = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            juce::MemoryBlock frame;
            juce::uint32 currentFrameNumber = 0;
            {
                const juce::ScopedLock lock(frameLock);
                currentFrameNumber = frameNumber;
                if (currentFrameNumber != sentFrameNumber && latestJpeg.getSize() > 0)
                    frame = latestJpeg;
            }

            if (frame.getSize() > 0)
            {
                if (! sendFrame(frame))
                    break;
                sentFrameNumber = currentFrameNumber;
                lastFrameAt = juce::Time::getMillisecondCounterHiRes();
            }
            else if (juce::Time::getMillisecondCounterHiRes() - lastFrameAt > 5000.0)
            {
                setStatus(Status::cameraUnavailable);
                break;
            }

            wait(frameIntervalMs);
        }

        socket.close();
        camera->removeListener(this);
        camera.reset();
        {
            const juce::ScopedLock lock(frameLock);
            activeCamera.clear();
        }

        if (! threadShouldExit())
        {
            setStatus(Status::connecting);
            wait(1000);
        }
    }
#endif
}

#if SONOBUS_CAMERA_SUPPORTED
void VideoRelayClient::imageReceived(const juce::Image& image)
{
    const auto jpeg = encodeJpeg(image);
    if (jpeg.getSize() == 0)
        return;

    const juce::ScopedLock lock(frameLock);
    latestJpeg = jpeg;
    ++frameNumber;
}
#endif

bool VideoRelayClient::connectWebSocket()
{
    juce::String localHost;
    juce::String localGroup;
    juce::String localUser;
    {
        const juce::ScopedLock lock(configLock);
        localHost = host;
        localGroup = group;
        localUser = user;
    }
    juce::String localCamera;
    {
        const juce::ScopedLock lock(frameLock);
        localCamera = activeCamera;
    }

    socket.setTimeout(5000);
    if (! socket.connect(localHost, videoPort, 5000))
        return false;

    juce::MemoryBlock nonce;
    nonce.setSize(16);
    auto& random = juce::Random::getSystemRandom();
    auto* nonceBytes = static_cast<juce::uint8*>(nonce.getData());
    for (size_t i = 0; i < nonce.getSize(); ++i)
        nonceBytes[i] = static_cast<juce::uint8>(random.nextInt(256));

    const auto key = juce::Base64::toBase64(nonce.getData(), nonce.getSize());
    const auto request = "GET /video/publish?room=" + escapeQuery(makeRoomName(localGroup))
                       + "&user=" + escapeQuery(localUser)
                       + "&camera=" + escapeQuery(localCamera) + " HTTP/1.1\r\n"
                         "Host: " + localHost + ":19090\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: " + key + "\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";

    const auto* requestBytes = request.toRawUTF8();
    if (socket.write(requestBytes, static_cast<int>(request.getNumBytesAsUTF8())) < 0)
        return false;

    juce::String response;
    char buffer[512];
    while (! response.contains("\r\n\r\n"))
    {
        if (socket.waitUntilReady(true, 5000) <= 0)
            return false;
        const auto bytesRead = socket.read(buffer, static_cast<int>(sizeof(buffer)), false);
        if (bytesRead <= 0)
            return false;
        response += juce::String::fromUTF8(buffer, bytesRead);
        if (response.length() > 8192)
            return false;
    }

    return response.startsWith("HTTP/1.1 101") || response.startsWith("HTTP/1.0 101");
}

bool VideoRelayClient::sendFrame(const juce::MemoryBlock& jpeg)
{
    juce::String localHost;
    juce::String localGroup;
    juce::String localUser;
    {
        const juce::ScopedLock lock(configLock);
        localHost = host;
        localGroup = group;
        localUser = user;
    }

    return sendMediaFrame(localHost, localGroup, localUser, mediaFrameNumber++, jpeg);
}

bool VideoRelayClient::sendMediaFrame(const juce::String& localHost,
                                       const juce::String& localGroup,
                                       const juce::String& localUser,
                                       juce::uint32 frameId,
                                       const juce::MemoryBlock& jpeg)
{
    const auto room = makeRoomName(localGroup);
    const auto roomBytes = room.getNumBytesAsUTF8();
    const auto userBytes = localUser.getNumBytesAsUTF8();
    if (roomBytes <= 0 || roomBytes > 255 || userBytes <= 0 || userBytes > 255 || jpeg.getSize() == 0)
        return false;

    const auto chunkCount = (jpeg.getSize() + mediaChunkPayloadBytes - 1) / mediaChunkPayloadBytes;
    if (chunkCount == 0 || chunkCount > 0xffff)
        return false;

    const auto* roomData = room.toRawUTF8();
    const auto* userData = localUser.toRawUTF8();
    const auto* source = static_cast<const juce::uint8*>(jpeg.getData());
    for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
    {
        const auto payloadOffset = chunkIndex * mediaChunkPayloadBytes;
        const auto payloadSize = jmin(mediaChunkPayloadBytes, jpeg.getSize() - payloadOffset);
        const auto packetSize = mediaHeaderBytes + roomBytes + userBytes + payloadSize;
        juce::MemoryBlock packet(packetSize);
        auto* bytes = static_cast<juce::uint8*>(packet.getData());
        std::memcpy(bytes, "SBV1", 4);
        bytes[4] = static_cast<juce::uint8>(roomBytes);
        bytes[5] = static_cast<juce::uint8>(userBytes);
        bytes[6] = static_cast<juce::uint8>((frameId >> 24) & 0xff);
        bytes[7] = static_cast<juce::uint8>((frameId >> 16) & 0xff);
        bytes[8] = static_cast<juce::uint8>((frameId >> 8) & 0xff);
        bytes[9] = static_cast<juce::uint8>(frameId & 0xff);
        bytes[10] = static_cast<juce::uint8>((chunkIndex >> 8) & 0xff);
        bytes[11] = static_cast<juce::uint8>(chunkIndex & 0xff);
        bytes[12] = static_cast<juce::uint8>((chunkCount >> 8) & 0xff);
        bytes[13] = static_cast<juce::uint8>(chunkCount & 0xff);
        std::memcpy(bytes + mediaHeaderBytes, roomData, static_cast<size_t>(roomBytes));
        std::memcpy(bytes + mediaHeaderBytes + roomBytes, userData, static_cast<size_t>(userBytes));
        std::memcpy(bytes + mediaHeaderBytes + roomBytes + userBytes, source + payloadOffset, payloadSize);

        if (mediaSocket.write(localHost, videoMediaPort, packet.getData(), static_cast<int>(packetSize)) != static_cast<int>(packetSize))
            return false;
    }
    return true;
}

void VideoRelayClient::setStatus(Status newStatus)
{
    status.store(newStatus);
}

juce::String VideoRelayClient::getStatusText() const
{
    switch (getStatus())
    {
        case Status::connecting:        return TRANS("连接中");
        case Status::online:            return TRANS("视频在线");
        case Status::cameraUnavailable: return TRANS("摄像头不可用");
        case Status::error:             return TRANS("视频连接失败");
        case Status::idle:              break;
    }
    return TRANS("未连接");
}

juce::String VideoRelayClient::getActiveCamera() const
{
    const juce::ScopedLock lock(frameLock);
    return activeCamera;
}

juce::String VideoRelayClient::escapeQuery(const juce::String& value)
{
    return juce::URL::addEscapeChars(value, false);
}

juce::MemoryBlock VideoRelayClient::encodeJpeg(const juce::Image& image)
{
    juce::MemoryOutputStream output;
    juce::JPEGImageFormat format;
    if (! format.writeImageToStream(image, output, 7))
        return {};

    juce::MemoryBlock result;
    result.append(output.getData(), output.getDataSize());
    return result;
}
