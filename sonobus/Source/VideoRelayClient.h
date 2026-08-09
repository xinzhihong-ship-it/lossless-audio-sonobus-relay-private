// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <JuceHeader.h>

class VideoRelayClient final : private juce::Thread
{
public:
    enum class Status
    {
        idle,
        unpaired,
        connecting,
        waitingForAdmin,
        startingCamera,
        online,
        cameraUnavailable,
        error
    };

    VideoRelayClient();
    ~VideoRelayClient() override;

    void start(const juce::String& host,
               const juce::String& group,
               const juce::String& user,
               const juce::String& pairingId,
               const juce::MemoryBlock& pairingKey);
    void stop();

    Status getStatus() const noexcept { return status.load(); }
    juce::String getStatusText() const;
    juce::String getActiveCamera() const;

    static bool parsePairingText(const juce::String& text,
                                 juce::String& pairingId,
                                 juce::MemoryBlock& pairingKey);

private:
    struct CameraDevice
    {
        juce::String id;
        juce::String name;
    };

    struct CameraMode
    {
        int width = 0;
        int height = 0;
        bool isValid() const noexcept { return width > 0 && height > 0; }
        bool operator==(const CameraMode& other) const noexcept { return width == other.width && height == other.height; }
        bool operator!=(const CameraMode& other) const noexcept { return !(*this == other); }
    };

    struct DesiredState
    {
        bool enabled = false;
        juce::String cameraDeviceId;
        juce::String ingestPath;
        juce::String publishUser;
        juce::String publishNonce;
        int rtspPort = 19092;
        juce::String revision;
    };

    void run() override;
    bool pollControl(DesiredState& desired, int& pollAfterMs);
    juce::var makeStatusPayload() const;
    juce::Array<CameraDevice> getCameraDevices(const juce::String& ffmpegPath) const;
    juce::Array<CameraMode> get60FpsCameraModes(const juce::String& ffmpegPath,
                                                const juce::String& cameraDeviceId) const;
    CameraMode findHighest60FpsMode(const juce::String& ffmpegPath,
                                    const juce::String& cameraDeviceId);
    bool probeCameraMode(const juce::String& ffmpegPath,
                         const juce::String& cameraDeviceId,
                         CameraMode mode) const;
    bool probeEncoder(const juce::String& ffmpegPath,
                      const juce::String& cameraDeviceId,
                      CameraMode mode,
                      const juce::String& encoder) const;
    bool startPublisher(const juce::String& ffmpegPath,
                        const juce::Array<CameraDevice>& devices,
                        const DesiredState& desired,
                        CameraMode mode);
    void stopPublisher();
    void readPublisherProgress();
    juce::String findFfmpeg() const;
    juce::StringArray availableEncoders(const juce::String& ffmpegPath) const;
    juce::StringArray captureArguments(const juce::String& cameraDeviceId, CameraMode mode) const;
    juce::StringArray encoderArguments(const juce::String& encoder) const;
    juce::StringArray publisherArguments(const juce::String& ffmpegPath,
                                         const juce::String& cameraDeviceId,
                                         CameraMode mode,
                                         const juce::String& encoder,
                                         const DesiredState& desired) const;
    void setStatus(Status newStatus, const juce::String& error = {});

    static juce::String hmacSha256Base64Url(const juce::MemoryBlock& key, const juce::String& value);
    static juce::String toBase64Url(const void* data, size_t size);
    static bool fromBase64Url(const juce::String& value, juce::MemoryBlock& result);
    static juce::String percentEncode(const juce::String& value);
    static bool secureEquals(const juce::String& left, const juce::String& right);

    mutable juce::CriticalSection stateLock;
    juce::String host;
    juce::String group;
    juce::String user;
    juce::String pairingId;
    juce::MemoryBlock pairingKey;
    juce::String clientId;
    juce::Array<CameraDevice> cameras;
    juce::String activeCameraId;
    juce::String activeCamera;
    juce::String activeEncoder;
    juce::String lastError;
    CameraMode activeMode;
    double actualFps = 0.0;
    int actualBitrate = 0;
    juce::uint64 sequence = 0;
    juce::String progressBuffer;

    std::atomic<Status> status { Status::idle };
    std::unique_ptr<juce::ChildProcess> publisher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRelayClient)
};
