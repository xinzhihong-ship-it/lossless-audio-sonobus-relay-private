// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <juce_core/juce_core.h>

namespace sonobus::video
{
struct CameraDevice
{
    juce::String id;
    juce::String name;
    // Internal capture detail; never included in the status payload.
    juce::String captureFfmpegPath;
};

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
// True only for devices intended for the private molixiu.exe frame bridge;
// known YYAnchor DirectShow filters stay on the ordinary DirectShow path.
bool isMoLiXiuBridgeCamera(const juce::String& id, const juce::String& name);
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
