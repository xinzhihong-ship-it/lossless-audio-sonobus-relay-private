// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include "VideoRelayClient.h"
#include <cstring>
#include <iterator>
#include <algorithm>
#include <regex>

#if JUCE_MAC
 #include <dlfcn.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
constexpr int controlPort = 19090;
constexpr int targetFps = 60;
constexpr int pollFallbackMs = 1000;

void moduleAnchor() {}

juce::File moduleFile()
{
#if JUCE_MAC
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&moduleAnchor), &info) != 0 && info.dli_fname != nullptr)
        return juce::File(juce::String::fromUTF8(info.dli_fname));
#elif JUCE_WINDOWS
    HMODULE module = nullptr;
    const auto* address = reinterpret_cast<const void*>(&moduleAnchor);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) != 0)
    {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (length > 0)
            return juce::File(juce::String(path, static_cast<int>(length)));
    }
#endif
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
}

juce::String formatHost(const juce::String& host)
{
    return host.containsChar(':') && ! host.startsWithChar('[') ? "[" + host + "]" : host;
}

int bitrateFor(int width, int height)
{
    const auto pixels = static_cast<juce::int64>(width) * height;
    if (pixels >= 3840LL * 2160LL) return 20000000;
    if (pixels >= 2560LL * 1440LL) return 12000000;
    if (pixels >= 1920LL * 1080LL) return 8000000;
    if (pixels >= 1280LL * 720LL) return 5000000;
    return 3000000;
}

juce::String tail(const juce::String& text, int maxLength = 500)
{
    const auto lines = juce::StringArray::fromLines(text.trim());
    return lines.isEmpty() ? text.trim().substring(0, maxLength) : lines[lines.size() - 1].substring(0, maxLength);
}
}

VideoRelayClient::VideoRelayClient()
    : juce::Thread("SonoBus H264 video control")
{
}

VideoRelayClient::~VideoRelayClient()
{
    stop();
}

void VideoRelayClient::start(const juce::String& host_,
                             const juce::String& group_,
                             const juce::String& user_,
                             const juce::String& pairingId_,
                             const juce::MemoryBlock& pairingKey_)
{
    stop();
    {
        const juce::ScopedLock lock(stateLock);
        host = host_.trim();
        group = group_.trim();
        user = user_.trim();
        pairingId = pairingId_.trim();
        pairingKey = pairingKey_;
        clientId = juce::Uuid().toString();
        sequence = 0;
        lastError.clear();
    }
    if (host.isEmpty() || group.isEmpty() || user.isEmpty() || pairingId.isEmpty() || pairingKey.getSize() != 32)
    {
        setStatus(Status::unpaired, TRANS("请先输入管理员生成的摄像头配对信息"));
        return;
    }
    setStatus(Status::connecting);
    startThread();
}

void VideoRelayClient::stop()
{
    signalThreadShouldExit();
    stopPublisher();
    stopThread(15000);
    {
        const juce::ScopedLock lock(stateLock);
        cameras.clear();
        activeCameraId.clear();
        activeCamera.clear();
        activeEncoder.clear();
        activeMode = {};
        actualFps = 0.0;
        actualBitrate = 0;
        progressBuffer.clear();
    }
    status.store(Status::idle);
}

void VideoRelayClient::run()
{
#if ! SONOBUS_CAMERA_SUPPORTED
    setStatus(Status::cameraUnavailable, TRANS("此平台构建不支持摄像头"));
    while (! threadShouldExit()) wait(500);
#else
    const auto ffmpegPath = findFfmpeg();
    if (ffmpegPath.isEmpty())
        setStatus(Status::error, TRANS("安装包中缺少 FFmpeg 视频运行时"));

    auto devices = ffmpegPath.isNotEmpty() ? getCameraDevices(ffmpegPath) : juce::Array<CameraDevice>();
    auto lastDeviceRefresh = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedLock lock(stateLock);
        cameras = devices;
    }
    DesiredState runningDesired;
    CameraMode runningMode;
    while (! threadShouldExit())
    {
        DesiredState desired;
        int pollAfterMs = pollFallbackMs;
        if (! pollControl(desired, pollAfterMs))
        {
            stopPublisher();
            setStatus(Status::error, lastError.isNotEmpty() ? lastError : TRANS("摄像头控制连接失败"));
            wait(juce::jlimit(500, 5000, pollAfterMs));
            continue;
        }

        if (ffmpegPath.isEmpty())
        {
            stopPublisher();
            setStatus(Status::error, TRANS("安装包中缺少 FFmpeg 视频运行时"));
            wait(juce::jlimit(500, 5000, pollAfterMs));
            continue;
        }

        const auto now = juce::Time::getMillisecondCounter();
        bool selectedMissing = desired.cameraDeviceId.isNotEmpty();
        for (const auto& device : devices)
            if (device.id == desired.cameraDeviceId) selectedMissing = false;
        if (devices.isEmpty() || selectedMissing || now - lastDeviceRefresh >= 5000)
        {
            devices = getCameraDevices(ffmpegPath);
            lastDeviceRefresh = now;
            const juce::ScopedLock lock(stateLock);
            cameras = devices;
        }

        if (! desired.enabled)
        {
            stopPublisher();
            runningDesired = desired;
            runningMode = {};
            setStatus(Status::waitingForAdmin);
        }
        else if (devices.isEmpty())
        {
            stopPublisher();
            setStatus(Status::cameraUnavailable, TRANS("未检测到摄像头"));
        }
        else
        {
            auto selectedCamera = desired.cameraDeviceId;
            if (selectedCamera.isEmpty()) selectedCamera = devices.getFirst().id;
            bool cameraAvailable = false;
            for (const auto& device : devices)
                if (device.id == selectedCamera) cameraAvailable = true;
            if (! cameraAvailable)
            {
                stopPublisher();
                setStatus(Status::cameraUnavailable, TRANS("管理员选择的摄像头当前不可用"));
            }
            else
            {
                const bool desiredChanged = selectedCamera != runningDesired.cameraDeviceId
                                         || desired.ingestPath != runningDesired.ingestPath
                                         || desired.publishNonce != runningDesired.publishNonce
                                         || desired.publishUser != runningDesired.publishUser
                                         || desired.rtspPort != runningDesired.rtspPort;
                bool hasPublisher = false;
                bool publisherRunning = false;
                {
                    const juce::ScopedLock lock(stateLock);
                    hasPublisher = publisher != nullptr;
                    publisherRunning = publisher != nullptr && publisher->isRunning();
                }
                if (hasPublisher && ! publisherRunning)
                {
                    readPublisherProgress();
                    stopPublisher();
                    setStatus(Status::error, lastError.isNotEmpty() ? lastError : TRANS("H.264 编码进程已退出"));
                    hasPublisher = false;
                }
                if (! hasPublisher || desiredChanged)
                {
                    stopPublisher();
                    setStatus(Status::startingCamera);
                    auto mode = selectedCamera == runningDesired.cameraDeviceId && runningMode.isValid()
                                  ? runningMode
                                  : findHighest60FpsMode(ffmpegPath, selectedCamera);
                    if (! mode.isValid())
                    {
                        setStatus(Status::cameraUnavailable, TRANS("摄像头没有可用的 60 FPS 模式"));
                    }
                    else
                    {
                        auto launchDesired = desired;
                        launchDesired.cameraDeviceId = selectedCamera;
                        if (startPublisher(ffmpegPath, devices, launchDesired, mode))
                        {
                            runningDesired = launchDesired;
                            runningMode = mode;
                            setStatus(Status::online);
                        }
                        else
                        {
                            setStatus(Status::error, lastError.isNotEmpty() ? lastError : TRANS("无法启动 H.264 硬件编码"));
                        }
                    }
                }
                readPublisherProgress();
            }
        }

        const auto sleepMs = juce::jlimit(250, 5000, pollAfterMs);
        for (int waited = 0; waited < sleepMs && ! threadShouldExit(); waited += 100)
        {
            readPublisherProgress();
            wait(juce::jmin(100, sleepMs - waited));
        }
    }
    stopPublisher();
#endif
}

bool VideoRelayClient::pollControl(DesiredState& desired, int& pollAfterMs)
{
    juce::String localHost, localGroup, localPairingId, localClientId;
    juce::MemoryBlock localKey;
    juce::uint64 localSequence = 0;
    {
        const juce::ScopedLock lock(stateLock);
        localHost = host;
        localGroup = group;
        localPairingId = pairingId;
        localClientId = clientId;
        localKey = pairingKey;
        localSequence = ++sequence;
    }

    const auto statusJson = juce::JSON::toString(makeStatusPayload(), true);
    const auto statusPayload = toBase64Url(statusJson.toRawUTF8(), statusJson.getNumBytesAsUTF8());
    juce::MemoryBlock nonceBytes(18, true);
    auto& random = juce::Random::getSystemRandom();
    for (size_t i = 0; i < nonceBytes.getSize(); ++i)
        static_cast<juce::uint8*>(nonceBytes.getData())[i] = static_cast<juce::uint8>(random.nextInt(256));
    const auto nonce = toBase64Url(nonceBytes.getData(), nonceBytes.getSize());
    const auto timestamp = juce::Time::currentTimeMillis();
    const auto signatureInput = "poll\n" + localPairingId + "\n" + localClientId + "\n"
                              + juce::String(timestamp) + "\n" + juce::String(localSequence) + "\n"
                              + nonce + "\n" + statusPayload;

    auto request = std::make_unique<juce::DynamicObject>();
    request->setProperty("pairingId", localPairingId);
    request->setProperty("clientId", localClientId);
    request->setProperty("timestamp", static_cast<juce::int64>(timestamp));
    request->setProperty("sequence", static_cast<juce::int64>(localSequence));
    request->setProperty("nonce", nonce);
    request->setProperty("status", statusPayload);
    request->setProperty("signature", hmacSha256Base64Url(localKey, signatureInput));
    const auto body = juce::JSON::toString(juce::var(request.release()), true);

    int statusCode = 0;
    juce::StringPairArray responseHeaders;
    auto endpoint = juce::URL("http://" + formatHost(localHost) + ":" + juce::String(controlPort) + "/video/control/poll").withPOSTData(body);
    auto stream = endpoint.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs(5000)
                                                 .withExtraHeaders("Content-Type: application/json\r\n")
                                                 .withResponseHeaders(&responseHeaders)
                                                 .withStatusCode(&statusCode)
                                                 .withNumRedirectsToFollow(0));
    if (stream == nullptr || statusCode != 200)
    {
        const juce::ScopedLock lock(stateLock);
        lastError = statusCode > 0 ? TRANS("控制服务器拒绝请求，HTTP ") + juce::String(statusCode)
                                   : TRANS("无法连接摄像头控制服务器");
        return false;
    }

    const auto responseText = stream->readEntireStreamAsString();
    const auto response = juce::JSON::parse(responseText);
    auto* object = response.getDynamicObject();
    if (object == nullptr)
        return false;
    const auto payload = object->getProperty("payload").toString();
    const auto responseSignature = object->getProperty("signature").toString();
    const auto expected = hmacSha256Base64Url(localKey, "response\n" + nonce + "\n" + payload);
    if (! secureEquals(responseSignature, expected))
    {
        const juce::ScopedLock lock(stateLock);
        lastError = TRANS("控制服务器响应签名无效");
        return false;
    }

    juce::MemoryBlock decodedPayload;
    if (! fromBase64Url(payload, decodedPayload))
        return false;
    const auto desiredJson = juce::String::fromUTF8(static_cast<const char*>(decodedPayload.getData()), static_cast<int>(decodedPayload.getSize()));
    const auto parsed = juce::JSON::parse(desiredJson);
    auto* desiredObject = parsed.getDynamicObject();
    if (desiredObject == nullptr || desiredObject->getProperty("type").toString() != "desired")
        return false;

    const auto expectedRoom = localGroup.startsWith("SB_") ? localGroup : "SB_" + localGroup;
    if (desiredObject->getProperty("room").toString() != expectedRoom)
    {
        const juce::ScopedLock lock(stateLock);
        lastError = TRANS("配对群组与当前 SonoBus 群组不一致");
        return false;
    }

    desired.enabled = static_cast<bool>(desiredObject->getProperty("enabled"));
    desired.cameraDeviceId = desiredObject->getProperty("cameraDeviceId").toString();
    desired.ingestPath = desiredObject->getProperty("ingestPath").toString();
    desired.publishUser = desiredObject->getProperty("publishUser").toString();
    desired.publishNonce = desiredObject->getProperty("publishNonce").toString();
    desired.rtspPort = juce::jlimit(1, 65535, static_cast<int>(desiredObject->getProperty("rtspPort")));
    desired.revision = desiredObject->getProperty("revision").toString();
    pollAfterMs = juce::jlimit(250, 5000, static_cast<int>(object->getProperty("pollAfterMs")));
    {
        const juce::ScopedLock lock(stateLock);
        lastError.clear();
    }
    return desired.ingestPath.isNotEmpty() && desired.publishUser.isNotEmpty() && desired.publishNonce.isNotEmpty();
}

juce::var VideoRelayClient::makeStatusPayload() const
{
    auto result = std::make_unique<juce::DynamicObject>();
    result->setProperty("type", "status");
    juce::Array<juce::var> cameraList;
    const juce::ScopedLock lock(stateLock);
    for (const auto& camera : cameras)
    {
        auto item = std::make_unique<juce::DynamicObject>();
        item->setProperty("id", camera.id);
        item->setProperty("name", camera.name);
        cameraList.add(juce::var(item.release()));
    }
    result->setProperty("cameras", cameraList);
    result->setProperty("capturing", publisher != nullptr && publisher->isRunning());
    result->setProperty("cameraDeviceId", activeCameraId);
    result->setProperty("cameraName", activeCamera);
    result->setProperty("codec", activeEncoder);
    if (activeMode.isValid())
    {
        result->setProperty("width", activeMode.width);
        result->setProperty("height", activeMode.height);
    }
    if (actualFps > 0.0) result->setProperty("fps", actualFps);
    if (actualBitrate > 0) result->setProperty("bitrate", actualBitrate);
    if (lastError.isNotEmpty()) result->setProperty("error", lastError);
    return juce::var(result.release());
}

juce::Array<VideoRelayClient::CameraDevice> VideoRelayClient::getCameraDevices(const juce::String& ffmpegPath) const
{
    juce::Array<CameraDevice> result;
#if JUCE_MAC
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", "" };
#elif JUCE_WINDOWS
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy" };
#else
    juce::ignoreUnused(ffmpegPath);
    return result;
#endif
#if JUCE_MAC || JUCE_WINDOWS
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return result;
    if (! probe.waitForProcessToFinish(5000)) probe.kill();
    const auto lines = juce::StringArray::fromLines(probe.readAllProcessOutput());
    bool inVideoSection = false;
#if JUCE_WINDOWS
    juce::String pendingName;
    const auto quotedValue = [](const juce::String& line)
    {
        const auto first = line.indexOfChar('"');
        if (first < 0) return juce::String();
        const auto rest = line.substring(first + 1);
        const auto second = rest.indexOfChar('"');
        return second < 0 ? juce::String() : rest.substring(0, second);
    };
#endif
    for (const auto& line : lines)
    {
#if JUCE_MAC
        if (line.contains("AVFoundation video devices:")) { inVideoSection = true; continue; }
        if (line.contains("AVFoundation audio devices:")) { inVideoSection = false; continue; }
        if (! inVideoSection) continue;
        const auto open = line.lastIndexOf("[");
        const auto rest = open >= 0 ? line.substring(open + 1) : juce::String();
        const auto close = rest.indexOfChar(']');
        const auto id = close >= 0 ? rest.substring(0, close).trim() : juce::String();
        const auto name = close >= 0 ? rest.substring(close + 1).trim() : juce::String();
        if (id.containsOnly("0123456789") && name.isNotEmpty() && ! name.startsWithIgnoreCase("Capture screen"))
            result.add({ id, name });
#elif JUCE_WINDOWS
        if (line.contains("DirectShow video devices")) { inVideoSection = true; continue; }
        if (line.contains("DirectShow audio devices"))
        {
            if (pendingName.isNotEmpty()) result.add({ pendingName, pendingName });
            pendingName.clear();
            inVideoSection = false;
            continue;
        }
        if (! inVideoSection) continue;
        if (line.containsIgnoreCase("Alternative name") && pendingName.isNotEmpty())
        {
            const auto id = quotedValue(line);
            result.add({ id.isNotEmpty() ? id : pendingName, pendingName });
            pendingName.clear();
        }
        else if (line.contains("(video)") && line.containsChar('"'))
        {
            if (pendingName.isNotEmpty()) result.add({ pendingName, pendingName });
            pendingName = quotedValue(line);
        }
#endif
    }
#if JUCE_WINDOWS
    if (pendingName.isNotEmpty()) result.add({ pendingName, pendingName });
#endif
#endif
    return result;
}

juce::Array<VideoRelayClient::CameraMode> VideoRelayClient::get60FpsCameraModes(const juce::String& ffmpegPath,
                                                                                 const juce::String& cameraDeviceId) const
{
    juce::Array<CameraMode> result;
#if JUCE_MAC
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-loglevel", "info", "-f", "avfoundation",
                                         "-framerate", juce::String(targetFps), "-video_size", "1x1",
                                         "-i", cameraDeviceId + ":none", "-frames:v", "1", "-f", "null", "-" };
    const std::regex modePattern(R"((\d+)x(\d+)@\[\s*([0-9.]+)\s+([0-9.]+)\]fps)");
    constexpr int fpsCapture = 4;
#elif JUCE_WINDOWS
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-loglevel", "info", "-list_options", "true",
                                         "-f", "dshow", "-i", "video=" + cameraDeviceId };
    const std::regex modePattern(R"(s=(\d+)x(\d+)\s+fps=([0-9.]+))");
    constexpr int fpsCapture = 3;
#else
    juce::ignoreUnused(ffmpegPath, cameraDeviceId);
    return result;
#endif
#if JUCE_MAC || JUCE_WINDOWS
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return result;
    if (! probe.waitForProcessToFinish(5000)) probe.kill();
    const auto output = probe.readAllProcessOutput().toStdString();
    for (auto match = std::sregex_iterator(output.begin(), output.end(), modePattern); match != std::sregex_iterator(); ++match)
    {
        if (std::stod((*match)[fpsCapture].str()) < 59.0) continue;
        const CameraMode mode { std::stoi((*match)[1].str()), std::stoi((*match)[2].str()) };
        bool duplicate = false;
        for (const auto existing : result) if (existing == mode) duplicate = true;
        if (! duplicate) result.add(mode);
    }
    std::sort(result.begin(), result.end(), [](const CameraMode& left, const CameraMode& right)
    {
        const auto leftPixels = static_cast<juce::int64>(left.width) * left.height;
        const auto rightPixels = static_cast<juce::int64>(right.width) * right.height;
        return leftPixels == rightPixels ? left.width > right.width : leftPixels > rightPixels;
    });
#endif
    return result;
}

VideoRelayClient::CameraMode VideoRelayClient::findHighest60FpsMode(const juce::String& ffmpegPath,
                                                                    const juce::String& cameraDeviceId)
{
    for (const auto mode : get60FpsCameraModes(ffmpegPath, cameraDeviceId))
    {
        if (threadShouldExit()) break;
        if (probeCameraMode(ffmpegPath, cameraDeviceId, mode)) return mode;
    }
    return {};
}

bool VideoRelayClient::probeCameraMode(const juce::String& ffmpegPath,
                                       const juce::String& cameraDeviceId,
                                       CameraMode mode) const
{
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "error" };
    arguments.addArray(captureArguments(cameraDeviceId, mode));
    arguments.addArray({ "-frames:v", "30", "-an", "-f", "null", "-" });
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
    if (! probe.waitForProcessToFinish(4000))
    {
        probe.kill();
        return false;
    }
    return ! threadShouldExit() && probe.getExitCode() == 0;
}

bool VideoRelayClient::probeEncoder(const juce::String& ffmpegPath,
                                    const juce::String& cameraDeviceId,
                                    CameraMode mode,
                                    const juce::String& encoder) const
{
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "error" };
    arguments.addArray(captureArguments(cameraDeviceId, mode));
    arguments.addArray({ "-frames:v", "30", "-an", "-r", juce::String(targetFps), "-fps_mode", "cfr", "-c:v", encoder });
    arguments.addArray(encoderArguments(encoder));
    arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0", "-g", "30", "-f", "null", "-" });
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
    if (! probe.waitForProcessToFinish(5000))
    {
        probe.kill();
        return false;
    }
    return ! threadShouldExit() && probe.getExitCode() == 0;
}

bool VideoRelayClient::startPublisher(const juce::String& ffmpegPath,
                                      const juce::Array<CameraDevice>& devices,
                                      const DesiredState& desired,
                                      CameraMode mode)
{
    const auto encoders = availableEncoders(ffmpegPath);
#if JUCE_MAC
    const juce::StringArray preferred { "h264_videotoolbox", "libx264" };
#elif JUCE_WINDOWS
    const juce::StringArray preferred { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libx264" };
#else
    const juce::StringArray preferred { "libx264" };
#endif
    juce::String launchErrors;
    for (const auto& encoder : preferred)
    {
        if (threadShouldExit()) break;
        if (! encoders.contains(encoder) || ! probeEncoder(ffmpegPath, desired.cameraDeviceId, mode, encoder)) continue;
        auto process = std::make_unique<juce::ChildProcess>();
        const auto arguments = publisherArguments(ffmpegPath, desired.cameraDeviceId, mode, encoder, desired);
        if (! process->start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) continue;
        if (process->waitForProcessToFinish(1500))
        {
            launchErrors = tail(process->readAllProcessOutput());
            continue;
        }
        if (threadShouldExit())
        {
            process->kill();
            break;
        }
        auto cameraName = desired.cameraDeviceId;
        for (const auto& device : devices) if (device.id == desired.cameraDeviceId) cameraName = device.name;
        {
            const juce::ScopedLock lock(stateLock);
            publisher = std::move(process);
            activeCameraId = desired.cameraDeviceId;
            activeCamera = cameraName;
            activeEncoder = encoder;
            activeMode = mode;
            actualFps = 0.0;
            actualBitrate = 0;
            progressBuffer.clear();
            lastError.clear();
        }
        return true;
    }
    {
        const juce::ScopedLock lock(stateLock);
        lastError = launchErrors.isNotEmpty() ? launchErrors : TRANS("没有可用的 H.264 编码器或编码硬件");
    }
    return false;
}

void VideoRelayClient::stopPublisher()
{
    std::unique_ptr<juce::ChildProcess> process;
    {
        const juce::ScopedLock lock(stateLock);
        process = std::move(publisher);
    }
    if (process != nullptr && process->isRunning()) process->kill();
    const juce::ScopedLock lock(stateLock);
    activeCameraId.clear();
    activeCamera.clear();
    activeEncoder.clear();
    activeMode = {};
    actualFps = 0.0;
    actualBitrate = 0;
    progressBuffer.clear();
}

void VideoRelayClient::readPublisherProgress()
{
    const juce::ScopedLock lock(stateLock);
    if (publisher == nullptr) return;
    char buffer[4096];
    for (;;)
    {
        const auto count = publisher->readProcessOutput(buffer, static_cast<int>(sizeof(buffer)));
        if (count <= 0) break;
        progressBuffer += juce::String::fromUTF8(buffer, count);
    }
    for (;;)
    {
        const auto newline = progressBuffer.indexOfChar('\n');
        if (newline < 0) break;
        const auto line = progressBuffer.substring(0, newline).trim();
        progressBuffer = progressBuffer.substring(newline + 1);
        if (line.startsWith("fps=")) actualFps = line.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
        if (line.startsWith("bitrate="))
        {
            const auto value = line.fromFirstOccurrenceOf("=", false, false).retainCharacters("0123456789.");
            if (value.isNotEmpty()) actualBitrate = juce::roundToInt(value.getDoubleValue() * 1000.0);
        }
        if (! line.containsChar('=') && line.isNotEmpty()) lastError = line.substring(0, 500);
    }
}

juce::String VideoRelayClient::findFfmpeg() const
{
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("SONOBUS_FFMPEG_PATH", {});
    if (overridePath.isNotEmpty() && juce::File(overridePath).existsAsFile()) return overridePath;
#if JUCE_WINDOWS
    const auto fileName = juce::String("ffmpeg.exe");
#else
    const auto fileName = juce::String("ffmpeg");
#endif
    const auto module = moduleFile();
#if JUCE_MAC
    const auto helper = module.getParentDirectory().getSiblingFile("Helpers")
                              .getChildFile("SonoBusVideoHelper.app").getChildFile("Contents")
                              .getChildFile("MacOS").getChildFile("ffmpeg");
    if (helper.existsAsFile()) return helper.getFullPathName();
#endif
    const auto sibling = module.getSiblingFile(fileName);
    if (sibling.existsAsFile()) return sibling.getFullPathName();
#if JUCE_MAC
    const auto resources = module.getParentDirectory().getSiblingFile("Resources").getChildFile(fileName);
    if (resources.existsAsFile()) return resources.getFullPathName();
#endif
    return {};
}

juce::StringArray VideoRelayClient::availableEncoders(const juce::String& ffmpegPath) const
{
    juce::ChildProcess probe;
    if (! probe.start({ ffmpegPath, "-hide_banner", "-encoders" }, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return {};
    if (! probe.waitForProcessToFinish(5000))
    {
        probe.kill();
        return {};
    }
    const auto output = probe.readAllProcessOutput();
    juce::StringArray result;
    for (const auto& name : { "h264_videotoolbox", "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libx264" })
        if (output.containsWholeWord(name)) result.add(name);
    return result;
}

juce::StringArray VideoRelayClient::captureArguments(const juce::String& cameraDeviceId, CameraMode mode) const
{
#if JUCE_MAC
    return { "-f", "avfoundation", "-framerate", juce::String(targetFps),
             "-video_size", juce::String(mode.width) + "x" + juce::String(mode.height),
             "-use_wallclock_as_timestamps", "1", "-i", cameraDeviceId + ":none" };
#elif JUCE_WINDOWS
    return { "-f", "dshow", "-rtbufsize", "256M", "-framerate", juce::String(targetFps),
             "-video_size", juce::String(mode.width) + "x" + juce::String(mode.height),
             "-use_wallclock_as_timestamps", "1", "-i", "video=" + cameraDeviceId };
#else
    juce::ignoreUnused(cameraDeviceId, mode);
    return {};
#endif
}

juce::StringArray VideoRelayClient::encoderArguments(const juce::String& encoder) const
{
    if (encoder == "h264_videotoolbox") return { "-realtime", "1", "-prio_speed", "1" };
    if (encoder == "h264_nvenc") return { "-preset", "p1", "-tune", "ull", "-zerolatency", "1", "-delay", "0" };
    if (encoder == "h264_qsv") return { "-preset", "veryfast", "-look_ahead", "0" };
    if (encoder == "h264_amf") return { "-usage", "lowlatency", "-quality", "speed" };
    if (encoder == "libx264") return { "-preset", "ultrafast", "-tune", "zerolatency" };
    return {};
}

juce::StringArray VideoRelayClient::publisherArguments(const juce::String& ffmpegPath,
                                                        const juce::String& cameraDeviceId,
                                                        CameraMode mode,
                                                        const juce::String& encoder,
                                                        const DesiredState& desired) const
{
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "warning", "-nostdin" };
    arguments.addArray(captureArguments(cameraDeviceId, mode));
    arguments.addArray({ "-an", "-r", juce::String(targetFps), "-fps_mode", "cfr", "-c:v", encoder });
    arguments.addArray(encoderArguments(encoder));

    const auto bitrate = bitrateFor(mode.width, mode.height);
    arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0", "-g", "30",
                         "-b:v", juce::String(bitrate), "-maxrate", juce::String(bitrate),
                         "-bufsize", juce::String(bitrate / 2), "-progress", "pipe:1", "-nostats" });

    juce::String localHost, localPairingId;
    juce::MemoryBlock localKey;
    {
        const juce::ScopedLock lock(stateLock);
        localHost = host;
        localPairingId = pairingId;
        localKey = pairingKey;
    }
    const auto password = hmacSha256Base64Url(localKey, "publish\n" + localPairingId + "\n" + desired.publishNonce);
    auto safePathParts = juce::StringArray::fromTokens(desired.ingestPath, "/", "");
    for (auto& part : safePathParts) part = percentEncode(part);
    const auto url = "rtsp://" + percentEncode(desired.publishUser) + ":" + percentEncode(password) + "@"
                   + formatHost(localHost) + ":" + juce::String(desired.rtspPort) + "/" + safePathParts.joinIntoString("/");
    arguments.addArray({ "-muxdelay", "0", "-flush_packets", "1", "-f", "rtsp", "-rtsp_transport", "tcp", url });
    return arguments;
}

void VideoRelayClient::setStatus(Status newStatus, const juce::String& error)
{
    const juce::ScopedLock lock(stateLock);
    status.store(newStatus);
    if (error.isNotEmpty()) lastError = error;
    else if (newStatus == Status::online || newStatus == Status::waitingForAdmin || newStatus == Status::connecting)
        lastError.clear();
}

juce::String VideoRelayClient::getStatusText() const
{
    const juce::ScopedLock lock(stateLock);
    switch (getStatus())
    {
        case Status::unpaired:          return TRANS("未配对") + (lastError.isNotEmpty() ? " · " + lastError : "");
        case Status::connecting:        return TRANS("连接管理员控制服务中");
        case Status::waitingForAdmin:   return TRANS("已连接，等待管理员开启摄像头");
        case Status::startingCamera:    return TRANS("检测最高 60 FPS 模式并启动 H.264 编码");
        case Status::online:
        {
            auto text = TRANS("H.264 视频在线");
            if (activeMode.isValid()) text += " · " + juce::String(activeMode.width) + "×" + juce::String(activeMode.height);
            if (actualFps > 0.0) text += " · " + juce::String(actualFps, 1) + " FPS";
            if (actualBitrate > 0) text += " · " + juce::String(actualBitrate / 1000000.0, 1) + " Mbps";
            return text;
        }
        case Status::cameraUnavailable: return TRANS("摄像头不可用") + (lastError.isNotEmpty() ? " · " + lastError : "");
        case Status::error:             return TRANS("视频错误") + (lastError.isNotEmpty() ? " · " + lastError : "");
        case Status::idle:              break;
    }
    return TRANS("未连接");
}

juce::String VideoRelayClient::getActiveCamera() const
{
    const juce::ScopedLock lock(stateLock);
    return activeCamera;
}

bool VideoRelayClient::parsePairingText(const juce::String& text,
                                        juce::String& pairingIdOut,
                                        juce::MemoryBlock& pairingKeyOut)
{
    const auto parts = juce::StringArray::fromTokens(text.trim(), ".", "");
    if (parts.size() != 3 || parts[0] != "SBPAIR1" || parts[1].isEmpty()) return false;
    juce::MemoryBlock key;
    if (! fromBase64Url(parts[2], key) || key.getSize() != 32) return false;
    pairingIdOut = parts[1];
    pairingKeyOut = key;
    return true;
}

juce::String VideoRelayClient::hmacSha256Base64Url(const juce::MemoryBlock& key, const juce::String& value)
{
    juce::MemoryBlock normalizedKey = key;
    if (normalizedKey.getSize() > 64) normalizedKey = juce::SHA256(normalizedKey).getRawData();
    normalizedKey.ensureSize(64, true);
    juce::MemoryBlock inner(64 + value.getNumBytesAsUTF8(), true);
    juce::MemoryBlock outer(64 + 32, true);
    auto* innerBytes = static_cast<juce::uint8*>(inner.getData());
    auto* outerBytes = static_cast<juce::uint8*>(outer.getData());
    const auto* keyBytes = static_cast<const juce::uint8*>(normalizedKey.getData());
    for (size_t i = 0; i < 64; ++i)
    {
        innerBytes[i] = static_cast<juce::uint8>(keyBytes[i] ^ 0x36);
        outerBytes[i] = static_cast<juce::uint8>(keyBytes[i] ^ 0x5c);
    }
    const auto utf8Value = value.toUTF8();
    std::memcpy(innerBytes + 64, utf8Value.getAddress(), value.getNumBytesAsUTF8());
    const auto innerHash = juce::SHA256(inner).getRawData();
    std::memcpy(outerBytes + 64, innerHash.getData(), innerHash.getSize());
    const auto result = juce::SHA256(outer).getRawData();
    return toBase64Url(result.getData(), result.getSize());
}

juce::String VideoRelayClient::toBase64Url(const void* data, size_t size)
{
    return juce::Base64::toBase64(data, size).replaceCharacter('+', '-').replaceCharacter('/', '_').trimCharactersAtEnd("=");
}

bool VideoRelayClient::fromBase64Url(const juce::String& value, juce::MemoryBlock& result)
{
    auto base64 = value.replaceCharacter('-', '+').replaceCharacter('_', '/');
    while ((base64.length() % 4) != 0) base64 += "=";
    juce::MemoryOutputStream output;
    if (! juce::Base64::convertFromBase64(output, base64)) return false;
    result = output.getMemoryBlock();
    return true;
}

juce::String VideoRelayClient::percentEncode(const juce::String& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    juce::String result;
    const auto utf8 = value.toUTF8();
    const auto* bytes = reinterpret_cast<const juce::uint8*>(utf8.getAddress());
    for (size_t i = 0; i < value.getNumBytesAsUTF8(); ++i)
    {
        const auto c = bytes[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            result += juce::String::charToString(static_cast<juce::juce_wchar>(c));
        else
            result << '%' << juce::String::charToString(hex[(c >> 4) & 0xf]) << juce::String::charToString(hex[c & 0xf]);
    }
    return result;
}

bool VideoRelayClient::secureEquals(const juce::String& left, const juce::String& right)
{
    const auto leftBytes = left.toRawUTF8();
    const auto rightBytes = right.toRawUTF8();
    const auto leftSize = left.getNumBytesAsUTF8();
    const auto rightSize = right.getNumBytesAsUTF8();
    unsigned int difference = static_cast<unsigned int>(leftSize ^ rightSize);
    const auto length = juce::jmax(leftSize, rightSize);
    for (size_t i = 0; i < length; ++i)
    {
        const auto a = i < leftSize ? static_cast<unsigned char>(leftBytes[i]) : 0;
        const auto b = i < rightSize ? static_cast<unsigned char>(rightBytes[i]) : 0;
        difference |= static_cast<unsigned int>(a ^ b);
    }
    return difference == 0;
}
