// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception

#include "../Source/VideoRelaySupport.h"
#include "../../tools/windows-molixiu-bridge/SonoBusMoLiXiuDevice.h"
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
        "SONOBUS_CAMERA\tdshow:@device_sw_physical\tIntegrated Camera\r\n"
        "SONOBUS_CAMERA\tdshow:@device_sw_obs\tOBS Virtual Camera\r\n"
        "SONOBUS_CAMERA\tdshow:@device_sw_{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\yyanchorvcam\tYY Anchor Camera\r\n"
        "SONOBUS_MODE\t1920\t1080\t59.94\r\n"
        "SONOBUS_MODE\t1280\t720\t30\r\n"
        "SONOBUS_MODE\t640\t480\t15\r\n"
        "SONOBUS_MODE\t320\t240\t0\r\n");
    const auto devices = sonobus::video::parseWindowsCameraDevices(deviceFixture);
    const auto modes = sonobus::video::parseWindowsCameraModes(deviceFixture);
    const auto dshowMode = sonobus::video::parseDshowCameraMode(
        "Input #0, dshow, from 'video=@device_sw_...':\n"
        "Stream #0:0: Video: rawvideo, bgr24, 1920x1080, 25 fps, 25 tbr\n");
    bool ok = expect(devices.size() == 4, "helper protocol did not filter physical DirectShow duplicates");
    ok &= expect(devices.size() > 0 && devices[0].id == "group-id-1", "camera source-group ID was lost");
    ok &= expect(devices.size() == 4 && devices[2].id == "dshow:@device_sw_obs",
                 "known virtual DirectShow camera was filtered unexpectedly");
    ok &= expect(devices.size() == 4 && devices[3].id.containsIgnoreCase("yyanchorvcam"),
                 "YY Anchor DirectShow camera was filtered unexpectedly");
    ok &= expect(devices.size() == 4 && ! sonobus::video::isMoLiXiuBridgeCamera(devices[3].id, devices[3].name),
                 "YY Anchor fixture was routed to the private MoLiXiu bridge");
    ok &= expect(! sonobus::video::isKnownVirtualCamera("group-id-1", "Integrated Camera"),
                 "physical camera was classified as virtual");
    ok &= expect(sonobus::video::isKnownVirtualCamera("dshow:@device_sw_obs", "OBS Virtual Camera"),
                 "virtual camera was not classified as virtual");
    ok &= expect(sonobus::video::isKnownVirtualCamera("dshow:@device_sw_yyanchormulcam", "YY Anchor Camera"),
                 "YY Anchor multi-client camera was not classified as virtual");
    ok &= expect(! sonobus::video::isMoLiXiuBridgeCamera(
                     "dshow:@device_sw_{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\\\yyanchorvcam", u8"魔力秀"),
                 "YY Anchor camera was routed to the private MoLiXiu bridge");
    ok &= expect(! sonobus::video::isMoLiXiuBridgeCamera("dshow:@device_sw_yyanchormulvcam", "YY开播"),
                 "YY Anchor multi-client camera was routed to the private MoLiXiu bridge");
    ok &= expect(! sonobus::video::isMoLiXiuBridgeCamera("dshow:@device_sw_yyanchormulcam", "魔力秀"),
                 "YY Anchor multi-client camera alias was routed to the private MoLiXiu bridge");
    ok &= expect(sonobus::video::isMoLiXiuBridgeCamera("molixiu-camera", {}),
                 "MoLiXiu bridge camera marker was not recognized");
    ok &= expect(sonobus::video::isMoLiXiuBridgeCamera("ishow-camera", {}),
                 "iShow bridge camera marker was not recognized");
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
    ok &= expect(! sonobus::molixiu::shouldPersistCameraHint(L"@device:sw:{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\yyanchorvcam"),
                 "virtual MoLiXiu output was accepted as a physical fallback");
    const std::wstring_view virtualHints[] {
        L"@device:sw\\YYANCHORVCAM",
        L"@device:sw\\yyanchormulvcam",
        L"@device:sw\\yyanchormulcam",
        L"OBS Virtual Camera",
        L"WebcastMate VirtualCamera",
        L"WebcastMate Virtual Camera",
        L"YY开播plus",
        L"魔力秀虚拟摄像头"
    };
    for (const auto hint : virtualHints)
        ok &= expect(! sonobus::molixiu::shouldPersistCameraHint(hint),
                     "known virtual camera hint was accepted as a physical fallback");
    ok &= expect(sonobus::molixiu::shouldPersistCameraHint(L"@device_pnp_\\\\?\\usb#vid_046d&pid_0825#罗技摄像头"),
                 "Unicode physical camera hint was rejected");
    const std::wstring_view molixiuHint = LR"(@device:pnp:\\?\usb#vid_5986&pid_212b&mi_00#7&39571d6c&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\global)";
    const std::wstring_view sourceGroup = LR"(\\?\USB#VID_5986&PID_212B&MI_00#7&39571D6C&0&0000#{e5323777-f976-4f5b-b94699c46e444}\GLOBAL)";
    ok &= expect(sonobus::molixiu::samePhysicalCameraSourceGroup(molixiuHint, sourceGroup),
                 "physical camera hint did not match its MediaCapture source group");
    ok &= expect(! sonobus::molixiu::samePhysicalCameraSourceGroup(
                     molixiuHint, LR"(\\?\USB#VID_046D&PID_0825#other#{e5323777-f976-4f5b-b94699c46e444}\GLOBAL)"),
                 "different physical camera paths were treated as the same source group");
    return ok ? 0 : 1;
}
