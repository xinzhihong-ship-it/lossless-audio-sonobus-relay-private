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
        "SONOBUS_MODE\t1280\t720\t30\r\n"
        "SONOBUS_MODE\t640\t480\t15\r\n"
        "SONOBUS_MODE\t320\t240\t0\r\n");
    const auto devices = sonobus::video::parseWindowsCameraDevices(deviceFixture);
    const auto modes = sonobus::video::parseWindowsCameraModes(deviceFixture);
    const auto dshowMode = sonobus::video::parseDshowCameraMode(
        "Input #0, dshow, from 'video=@device_sw_...':\n"
        "Stream #0:0: Video: rawvideo, bgr24, 1920x1080, 25 fps, 25 tbr\n");
    bool ok = expect(devices.size() == 2, "helper protocol did not find two video devices");
    ok &= expect(devices.size() > 0 && devices[0].id == "group-id-1", "camera source-group ID was lost");
    ok &= expect(devices.size() > 0 && devices[0].name == "Integrated Camera", "friendly camera name was lost");
    ok &= expect(modes.size() == 3 && modes[0].fps >= 59.0 && modes[1].fps == 30.0 && modes[2].fps == 15.0,
                 "helper protocol did not preserve real source FPS modes or accepted an invalid mode");
    ok &= expect(dshowMode.width == 1920 && dshowMode.height == 1080 && dshowMode.fps == 25.0,
                 "DirectShow probe did not preserve the actual input mode");
    const auto limited = sonobus::video::constrainOutputMode({ 1920, 1080, 59.94 }, 720, 30.0);
    ok &= expect(limited.width == 1280 && limited.height == 720 && limited.fps == 30.0,
                 "output limits did not downscale and drop FPS without upsampling");
    const auto unchanged = sonobus::video::constrainOutputMode({ 1280, 720, 30.0 }, 2160, 60.0);
    ok &= expect(unchanged.width == 1280 && unchanged.height == 720 && unchanged.fps == 30.0,
                 "output limits enlarged or duplicated the source mode");
    ok &= expect(sonobus::video::classifyCameraFailure("Could not run graph (device is already in use)") == sonobus::video::CameraFailure::busy,
                 "camera busy error was not classified");
    ok &= expect(sonobus::video::classifyCameraFailure("Access is denied 0x80070005") == sonobus::video::CameraFailure::permissionDenied,
                 "camera privacy error was not classified");
    ok &= expect(sonobus::video::utf8(u8"状态：摄像头 · 1280×720") == juce::String::fromUTF8(u8"状态：摄像头 · 1280×720"),
                 "explicit UTF-8 conversion failed");
    return ok ? 0 : 1;
}
