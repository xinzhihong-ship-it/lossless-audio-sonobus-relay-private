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

juce::Array<CameraDevice> parseWindowsCameraDevices(const juce::String& output);
juce::Array<CameraMode> parseWindowsCameraModes(const juce::String& output);
CameraMode parseDshowCameraMode(const juce::String& output);
CameraMode constrainOutputMode(CameraMode capture, int maxHeight, double maxFps);
CameraFailure classifyCameraFailure(const juce::String& output);
juce::String lastOutputLine(const juce::String& output, int maxLength = 500);
}
