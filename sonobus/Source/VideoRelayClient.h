// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <JuceHeader.h>
#include <juce_video/juce_video.h>

class VideoRelayClient final : private juce::Thread,
                               private juce::CameraDevice::Listener
{
public:
    enum class Status
    {
        idle,
        connecting,
        online,
        cameraUnavailable,
        error
    };

    VideoRelayClient();
    ~VideoRelayClient() override;

    static juce::StringArray getCameraDevices();

    void start(const juce::String& host,
               const juce::String& group,
               const juce::String& user,
               const juce::String& preferredCamera);
    void stop();

    Status getStatus() const noexcept { return status.load(); }
    juce::String getStatusText() const;
    juce::String getActiveCamera() const;

private:
    void run() override;
    void imageReceived(const juce::Image& image) override;

    bool connectWebSocket();
    bool sendFrame(const juce::MemoryBlock& jpeg);
    bool sendMediaFrame(const juce::String& localHost,
                        const juce::String& localGroup,
                        const juce::String& localUser,
                        juce::uint32 frameId,
                        const juce::MemoryBlock& jpeg);
    void setStatus(Status newStatus);

    static juce::String escapeQuery(const juce::String& value);
    static juce::MemoryBlock encodeJpeg(const juce::Image& image);

    juce::CriticalSection configLock;
    juce::String host;
    juce::String group;
    juce::String user;
    juce::String preferredCamera;

    juce::CriticalSection frameLock;
    juce::MemoryBlock latestJpeg;
    juce::String activeCamera;
    juce::uint32 frameNumber = 0;

    std::atomic<Status> status { Status::idle };
    std::unique_ptr<juce::CameraDevice> camera;
    juce::StreamingSocket socket;
    juce::DatagramSocket mediaSocket;
    juce::uint32 mediaFrameNumber = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRelayClient)
};
