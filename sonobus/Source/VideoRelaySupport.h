// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <juce_core/juce_core.h>

namespace sonobus::video
{
// Camera IDs are opaque to the control plane. Windows DirectShow selectors are
// stable identifiers but case-insensitive; match their complete value without
// fuzzy name or suffix matching.
bool sameCameraDeviceId(const juce::String& left, const juce::String& right);

// Administrator state used to decide whether a new publisher launch was requested.
// The enabled flag is handled by the caller, which resets the attempt when the
// administrator disables video.
struct PublisherControl
{
    bool enabled = false;
    juce::String cameraDeviceId, ingestPath, publishUser, publishNonce;
    int rtspPort = 19092;
    int maxHeight = 0;
    double maxFps = 0.0;
    int maxBitrate = 0;
    juce::String revision;

    bool sameLaunchAs(const PublisherControl& other) const
    {
        return sameCameraDeviceId(cameraDeviceId, other.cameraDeviceId) && ingestPath == other.ingestPath
            && publishUser == other.publishUser && publishNonce == other.publishNonce
            && rtspPort == other.rtspPort && maxHeight == other.maxHeight
            && maxFps == other.maxFps && maxBitrate == other.maxBitrate;
    }
};

struct PublisherAttempt
{
    PublisherControl control;
    bool recorded = false;

    bool changed(const PublisherControl& next) const
    {
        return ! recorded || next.revision != control.revision || ! next.sameLaunchAs(control);
    }

    void record(const PublisherControl& next)
    {
        control = next;
        recorded = true;
    }

    void reset()
    {
        control = {};
        recorded = false;
    }
};

struct CameraDevice
{
    juce::String id;
    juce::String name;
    // Internal capture detail; never included in the status payload.
    juce::String captureFfmpegPath;
};

int findCameraDeviceIndex(const juce::Array<CameraDevice>& devices, const juce::String& id);

struct CameraMode
{
    int width = 0;
    int height = 0;
    double fps = 0.0;
};

enum class CameraFailure
{
    none,
    busy,
    permissionDenied,
    unavailable
};

inline juce::String utf8(const char* text)
{
    return juce::String::fromUTF8(text);
}

inline juce::String translated(const char* text)
{
    return juce::translate(utf8(text));
}

bool isKnownVirtualCamera(const juce::String& id, const juce::String& name);
// Always false for case-insensitive dshow: IDs, before any friendly-name heuristics.
// Non-DirectShow bridge markers retain their existing classification; callers must
// separately validate virtual-camera eligibility and private molixiu.exe compatibility.
bool isMoLiXiuBridgeCamera(const juce::String& id, const juce::String& name);
// Preserve an explicitly selected camera; only the private MoLiXiu selection uses
// the synthetic bridge ID, never an unrelated enumerated virtual camera.
juce::String captureCameraForSelection(const juce::String& selectedId, bool selectedIsMoLiXiu);
// True while molixiu.exe is running. The relay must then leave the virtual
// cameras alone: MoLiXiu captures one of them itself, and a DirectShow open by
// the relay would take that source away from the application.
bool isMoLiXiuRunning();
juce::Array<CameraDevice> parseWindowsCameraDevices(const juce::String& output);
juce::Array<CameraMode> parseWindowsCameraModes(const juce::String& output);
CameraMode parseDshowCameraMode(const juce::String& output);
CameraMode constrainOutputMode(CameraMode capture, int maxHeight, double maxFps);
CameraFailure classifyCameraFailure(const juce::String& output);
juce::String lastOutputLine(const juce::String& output, int maxLength = 500);
}
