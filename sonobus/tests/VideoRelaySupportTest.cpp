// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception

#include "../Source/VideoRelaySupport.h"
#include <iostream>

namespace
{
bool expect(bool condition, const char* message)
{
    if (! condition) std::cerr << message << '\n';
    return condition;
}
}

int main()
{
    const auto deviceFixture = juce::String::fromUTF8(
        "diagnostic line\r\n"
        "SONOBUS_CAMERA\tgroup-id-1\tIntegrated Camera\r\n"
        "SONOBUS_CAMERA\tgroup-id-2\tOBS Virtual Camera\r\n"
        "SONOBUS_MODE\t1920\t1080\t59.94\r\n"
        "SONOBUS_MODE\t1280\t720\t30\r\n");
    const auto devices = sonobus::video::parseWindowsCameraDevices(deviceFixture);
    const auto modes = sonobus::video::parseWindowsCameraModes(deviceFixture);
    bool ok = expect(devices.size() == 2, "helper protocol did not find two video devices");
    ok &= expect(devices.size() > 0 && devices[0].id == "group-id-1", "camera source-group ID was lost");
    ok &= expect(devices.size() > 0 && devices[0].name == "Integrated Camera", "friendly camera name was lost");
    ok &= expect(modes.size() == 1 && modes[0].width == 1920 && modes[0].height == 1080,
                 "helper protocol accepted a non-60-FPS mode or lost the shared 60-FPS mode");
    ok &= expect(sonobus::video::classifyCameraFailure("Could not run graph (device is already in use)") == sonobus::video::CameraFailure::busy,
                 "camera busy error was not classified");
    ok &= expect(sonobus::video::classifyCameraFailure("Access is denied 0x80070005") == sonobus::video::CameraFailure::permissionDenied,
                 "camera privacy error was not classified");
    ok &= expect(sonobus::video::utf8(u8"状态：摄像头 · 1280×720") == juce::String::fromUTF8(u8"状态：摄像头 · 1280×720"),
                 "explicit UTF-8 conversion failed");
    return ok ? 0 : 1;
}
