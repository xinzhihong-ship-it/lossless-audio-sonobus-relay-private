// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include "VideoRelaySupport.h"

#if JUCE_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <regex>

namespace sonobus::video
{
namespace
{
juce::StringArray fields(const juce::String& line, const juce::String& prefix)
{
    return line.startsWith(prefix) ? juce::StringArray::fromTokens(line, "\t", "") : juce::StringArray();
}
}

bool isKnownVirtualCamera(const juce::String& id, const juce::String& name)
{
    const auto value = id + " " + name;
    return value.containsIgnoreCase("yyanchorvcam")
        || value.containsIgnoreCase("yyanchormulvcam")
        || value.containsIgnoreCase("yyanchormulcam")
        || value.containsIgnoreCase("obs virtual camera")
        || value.containsIgnoreCase("webcastmate virtualcamera")
        || value.containsIgnoreCase("virtual camera")
        || value.contains(u8"YY开播")
        || value.contains(u8"魔力秀");
}

bool isMoLiXiuBridgeCamera(const juce::String& id, const juce::String& name)
{
    // YYAnchor exposes a normal DirectShow filter. The private bridge is only
    // valid for a compatible molixiu.exe host; the device name alone must not
    // silently redirect YYAnchor to --publish-molixiu.
    const auto value = id + " " + name;
    return ! value.containsIgnoreCase("yyanchorvcam")
        && ! value.containsIgnoreCase("yyanchormulvcam")
        && ! value.containsIgnoreCase("yyanchormulcam")
        && (value.containsIgnoreCase("molixiu")
            || value.containsIgnoreCase("ishow")
            || value.contains(u8"YY开播")
            || value.contains(u8"魔力秀"));
}

juce::Array<CameraDevice> parseWindowsCameraDevices(const juce::String& output)
{
    juce::Array<CameraDevice> devices;
    for (const auto& line : juce::StringArray::fromLines(output))
    {
        const auto values = fields(line, "SONOBUS_CAMERA\t");
        if (values.size() >= 3 && values[1].isNotEmpty() && values[2].isNotEmpty()
            // A physical camera must stay on the MediaCapture SharedReadOnly path.
            // Keep DirectShow entries only when they are recognisable virtual filters.
            && (! values[1].startsWith("dshow:") || isKnownVirtualCamera(values[1], values[2])))
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
            && values[3].getDoubleValue() > 0.0)
            modes.add({ values[1].getIntValue(), values[2].getIntValue(), values[3].getDoubleValue() });
    }
    return modes;
}

CameraMode parseDshowCameraMode(const juce::String& output)
{
    const std::regex modePattern(R"((\d+)x(\d+),\s*([0-9]+(?:\.[0-9]+)?)\s*fps)");
    const auto text = output.toStdString();
    std::smatch match;
    if (! std::regex_search(text, match, modePattern)) return {};
    return { std::stoi(match[1].str()), std::stoi(match[2].str()), std::stod(match[3].str()) };
}

CameraMode constrainOutputMode(CameraMode capture, int maxHeight, double maxFps)
{
    if (capture.width <= 0 || capture.height <= 0 || capture.fps <= 0.0) return {};
    auto output = capture;
    if (maxHeight > 0 && maxHeight < output.height)
    {
        output.height = maxHeight - (maxHeight % 2);
        if (output.height < 2) output.height = 2;
        output.width = ((capture.width * output.height / capture.height) / 2) * 2;
        if (output.width < 2) output.width = 2;
    }
    if (maxFps > 0.0 && maxFps < output.fps) output.fps = maxFps;
    return output;
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

#if JUCE_WINDOWS
bool isMoLiXiuRunning()
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (juce::String(entry.szExeFile).equalsIgnoreCase("molixiu.exe"))
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}
#else
bool isMoLiXiuRunning() { return false; }
#endif
}
