// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include "VideoRelaySupport.h"

namespace sonobus::video
{
namespace
{
juce::StringArray fields(const juce::String& line, const juce::String& prefix)
{
    return line.startsWith(prefix) ? juce::StringArray::fromTokens(line, "\t", "") : juce::StringArray();
}
}

juce::Array<CameraDevice> parseWindowsCameraDevices(const juce::String& output)
{
    juce::Array<CameraDevice> devices;
    for (const auto& line : juce::StringArray::fromLines(output))
    {
        const auto values = fields(line, "SONOBUS_CAMERA\t");
        if (values.size() >= 3 && values[1].isNotEmpty() && values[2].isNotEmpty())
            devices.add({ values[1], values[2] });
    }
    return devices;
}

juce::Array<CameraMode> parseWindowsCameraModes(const juce::String& output)
{
    juce::Array<CameraMode> modes;
    for (const auto& line : juce::StringArray::fromLines(output))
    {
        const auto values = fields(line, "SONOBUS_MODE\t");
        if (values.size() >= 4 && values[1].getIntValue() > 0 && values[2].getIntValue() > 0
            && values[3].getDoubleValue() >= 59.0)
            modes.add({ values[1].getIntValue(), values[2].getIntValue(), values[3].getDoubleValue() });
    }
    return modes;
}

CameraFailure classifyCameraFailure(const juce::String& output)
{
    const auto text = output.toLowerCase();
    if (text.contains("already in use") || text.contains("device is in use")
        || text.contains("device busy") || text.contains("resource busy")
        || text.contains("could not run graph") || text.contains("0x80070020")
        || text.contains("0x800700aa") || text.contains("camerareservedbyanotherapp"))
        return CameraFailure::busy;
    if (text.contains("access is denied") || text.contains("permission denied")
        || text.contains("0x80070005") || text.contains("camera access is disabled"))
        return CameraFailure::permissionDenied;
    return text.trim().isEmpty() ? CameraFailure::none : CameraFailure::unavailable;
}

juce::String lastOutputLine(const juce::String& output, int maxLength)
{
    const auto lines = juce::StringArray::fromLines(output.trim());
    return lines.isEmpty() ? output.trim().substring(0, maxLength)
                           : lines[lines.size() - 1].trim().substring(0, maxLength);
}
}
