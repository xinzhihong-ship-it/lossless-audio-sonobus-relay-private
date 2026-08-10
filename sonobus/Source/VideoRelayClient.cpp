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

// Drains a child process without ever blocking the caller: reads output in
// small non-blocking chunks while the process runs, kills on timeout, then
// drains once more. Returns true when the process finished on its own.
bool runProbe(juce::ChildProcess& probe, int timeoutMs, juce::String& output)
{
    char buffer[4096];
    const auto deadline = juce::Time::getMillisecondCounter() + timeoutMs;
    while (probe.isRunning() && juce::Time::getMillisecondCounter() < deadline)
    {
        for (;;)
        {
            const auto count = probe.readProcessOutput(buffer, static_cast<int>(sizeof(buffer)));
            if (count <= 0) break;
            output += juce::String::fromUTF8(buffer, count);
        }
        juce::Thread::sleep(2);
    }
    const auto finished = ! probe.isRunning();
    if (! finished) probe.kill();
    for (;;)
    {
        const auto count = probe.readProcessOutput(buffer, static_cast<int>(sizeof(buffer)));
        if (count <= 0) break;
        output += juce::String::fromUTF8(buffer, count);
    }
    return finished;
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

juce::String cameraFailureMessage(const juce::String& output)
{
    if (output.containsIgnoreCase("SONOBUS_ERROR=busy"))
        return sonobus::video::translated(u8"摄像头被其他程序独占；请在后台选择另一台，或关闭占用程序后等待重试");
    if (output.containsIgnoreCase("SONOBUS_ERROR=permission"))
        return sonobus::video::translated(u8"Windows 已拒绝摄像头权限；请允许桌面应用访问摄像头");
    if (output.containsIgnoreCase("shared-frame-rate-below-expected"))
        return sonobus::video::translated(u8"摄像头共享流实测帧率低于当前模式标称值；请关闭占用程序后等待重试");
    if (output.containsIgnoreCase("SONOBUS_ERROR=unavailable:frame-timeout:"))
    {
        const auto detail = sonobus::video::lastOutputLine(output).fromFirstOccurrenceOf("frame-timeout:", false, false);
        return sonobus::video::translated(u8"摄像头共享帧超时")
             + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String());
    }
    if (output.containsIgnoreCase("SONOBUS_ERROR=unavailable:frame-timeout"))
        return sonobus::video::translated(u8"摄像头共享帧超时；请检查火绒和其他摄像头占用程序");
    if (output.containsIgnoreCase("SONOBUS_ERROR=unavailable:"))
    {
        const auto detail = sonobus::video::lastOutputLine(output).fromFirstOccurrenceOf(":", false, false);
        return sonobus::video::translated(u8"摄像头共享模式不可用")
             + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String());
    }
    using sonobus::video::CameraFailure;
    switch (sonobus::video::classifyCameraFailure(output))
    {
        case CameraFailure::busy:
            return sonobus::video::translated(u8"摄像头正被其他程序占用；请在后台选择另一台，或关闭占用程序后等待重试");
        case CameraFailure::permissionDenied:
            return sonobus::video::translated(u8"Windows 已拒绝摄像头权限；请允许桌面应用访问摄像头");
        case CameraFailure::unavailable:
            return sonobus::video::lastOutputLine(output);
        case CameraFailure::none:
            break;
    }
    return {};
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
                             const juce::MemoryBlock& pairingKey_,
                             const juce::String& enrollmentSecret_,
                             EnrollmentHandler enrollmentHandler_)
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
        juce::MemoryBlock secret;
        if (enrollmentSecret_.length() == 64) secret.loadFromHexString(enrollmentSecret_);
        enrollmentKey = secret.getSize() == 32 ? deriveEnrollmentKey(secret, group, user, clientId) : juce::MemoryBlock();
        enrollmentHandler = std::move(enrollmentHandler_);
        sequence = 0;
        pairingRejected = false;
        lastError.clear();
        cameraError.clear();
    }
    if (host.isEmpty() || group.isEmpty() || user.isEmpty())
    {
        setStatus(Status::unpaired, sonobus::video::translated(u8"请先加入 SonoBus 群组"));
        return;
    }
    if ((pairingId.isEmpty() || pairingKey.getSize() != 32) && enrollmentKey.getSize() != 32)
    {
        setStatus(Status::unpaired, sonobus::video::translated(u8"当前连接不支持自动授权；请断开并重新加入群组"));
        return;
    }
    setStatus(pairingId.isNotEmpty() && pairingKey.getSize() == 32 ? Status::connecting : Status::awaitingAuthorization);
    startThread();
}

void VideoRelayClient::stop()
{
    signalThreadShouldExit();
    stopPublisher();
    stopThread(5000);
    {
        const juce::ScopedLock lock(stateLock);
        cameras.clear();
        activeCameraId.clear();
        activeCamera.clear();
        activeEncoder.clear();
        activeMode = {};
        captureMode = {};
        captureFps = 0.0;
        actualFps = 0.0;
        actualBitrate = 0;
        progressBuffer.clear();
        cameraError.clear();
        enrollmentKey.reset();
        enrollmentHandler = {};
        pairingRejected = false;
    }
    status.store(Status::idle);
}

void VideoRelayClient::run()
{
#if ! SONOBUS_CAMERA_SUPPORTED
    setStatus(Status::cameraUnavailable, sonobus::video::translated(u8"此平台构建不支持摄像头"));
    while (! threadShouldExit()) wait(500);
#else
    const auto ffmpegPath = findFfmpeg();
    if (ffmpegPath.isEmpty())
        setStatus(Status::error, sonobus::video::translated(u8"安装包中缺少 FFmpeg 视频运行时"));

    juce::String enumerationError;
    auto devices = ffmpegPath.isNotEmpty() ? getCameraDevices(ffmpegPath, enumerationError) : juce::Array<CameraDevice>();
    auto lastDeviceRefresh = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedLock lock(stateLock);
        cameras = devices;
        cameraError = enumerationError;
    }
    DesiredState runningDesired;
    CameraMode runningMode;
    juce::String lastAttemptRevision;
    double nextCameraAttemptMs = 0.0;
    int retryDelayMs = 30000;
    double nextDeviceRefreshMs = 0.0;
    int deviceRefreshDelayMs = 1000;
    while (! threadShouldExit())
    {
        bool paired = false;
        {
            const juce::ScopedLock lock(stateLock);
            paired = pairingId.isNotEmpty() && pairingKey.getSize() == 32;
        }
        if (! paired)
        {
            int enrollmentPollMs = pollFallbackMs;
            if (! requestEnrollment(enrollmentPollMs))
                setStatus(Status::error, lastError.isNotEmpty() ? lastError
                    : sonobus::video::translated(u8"无法申请摄像头授权"));
            for (int waited = 0; waited < enrollmentPollMs && ! threadShouldExit(); waited += 100)
                wait(juce::jmin(100, enrollmentPollMs - waited));
            continue;
        }

        DesiredState desired;
        int pollAfterMs = pollFallbackMs;
        if (! pollControl(desired, pollAfterMs))
        {
            stopPublisher();
            bool enrollAgain = false;
            {
                const juce::ScopedLock lock(stateLock);
                enrollAgain = pairingRejected && enrollmentKey.getSize() == 32;
                if (enrollAgain)
                {
                    pairingId.clear();
                    pairingKey.reset();
                    sequence = 0;
                    pairingRejected = false;
                }
            }
            if (enrollAgain)
            {
                setStatus(Status::awaitingAuthorization);
                continue;
            }
            setStatus(Status::error, lastError.isNotEmpty() ? lastError : sonobus::video::translated(u8"摄像头控制连接失败"));
            wait(juce::jlimit(500, 5000, pollAfterMs));
            continue;
        }

        if (ffmpegPath.isEmpty())
        {
            stopPublisher();
            setStatus(Status::error, sonobus::video::translated(u8"安装包中缺少 FFmpeg 视频运行时"));
            wait(juce::jlimit(500, 5000, pollAfterMs));
            continue;
        }

        const auto now = juce::Time::getMillisecondCounter();
        const auto nowHi = juce::Time::getMillisecondCounterHiRes();
        bool selectedMissing = desired.cameraDeviceId.isNotEmpty();
        for (const auto& device : devices)
            if (device.id == desired.cameraDeviceId) selectedMissing = false;
        const bool unavailable = devices.isEmpty() || selectedMissing;
        if ((unavailable && nowHi >= nextDeviceRefreshMs) || (! unavailable && now - lastDeviceRefresh >= 5000))
        {
            enumerationError.clear();
            devices = getCameraDevices(ffmpegPath, enumerationError);
            lastDeviceRefresh = now;
            bool stillMissing = desired.cameraDeviceId.isNotEmpty();
            for (const auto& device : devices)
                if (device.id == desired.cameraDeviceId) stillMissing = false;
            if (devices.isEmpty() || stillMissing)
            {
                nextDeviceRefreshMs = nowHi + deviceRefreshDelayMs;
                deviceRefreshDelayMs = juce::jmin(deviceRefreshDelayMs * 2, 30000);
            }
            else
            {
                nextDeviceRefreshMs = 0.0;
                deviceRefreshDelayMs = 1000;
            }
            const juce::ScopedLock lock(stateLock);
            cameras = devices;
            cameraError = enumerationError;
        }

        if (! desired.enabled)
        {
            stopPublisher();
            runningDesired = desired;
            runningMode = {};
            lastAttemptRevision.clear();
            nextCameraAttemptMs = 0.0;
            retryDelayMs = 30000;
            setStatus(Status::waitingForAdmin);
        }
        else if (desired.cameraDeviceId.isEmpty())
        {
            stopPublisher();
            setStatus(Status::cameraUnavailable, sonobus::video::translated(u8"后台尚未选择摄像头"));
        }
        else if (devices.isEmpty())
        {
            stopPublisher();
            setStatus(Status::cameraUnavailable, enumerationError.isNotEmpty() ? enumerationError
                : sonobus::video::translated(u8"未检测到摄像头"));
        }
        else
        {
            const auto selectedCamera = desired.cameraDeviceId;
            bool cameraAvailable = false;
            for (const auto& device : devices)
                if (device.id == selectedCamera) cameraAvailable = true;
            if (! cameraAvailable)
            {
                stopPublisher();
                setStatus(Status::cameraUnavailable, sonobus::video::translated(u8"管理员选择的摄像头当前不可用"));
            }
            else
            {
                const bool desiredChanged = selectedCamera != runningDesired.cameraDeviceId
                                         || desired.ingestPath != runningDesired.ingestPath
                                         || desired.publishNonce != runningDesired.publishNonce
                                         || desired.publishUser != runningDesired.publishUser
                                         || desired.rtspPort != runningDesired.rtspPort
                                         || desired.maxHeight != runningDesired.maxHeight
                                         || desired.maxFps != runningDesired.maxFps
                                         || desired.maxBitrate != runningDesired.maxBitrate;
                const bool attemptChanged = desired.revision != lastAttemptRevision;
                if (attemptChanged)
                {
                    nextCameraAttemptMs = 0.0;
                    retryDelayMs = 30000;
                }
                const auto nowMs = juce::Time::getMillisecondCounterHiRes();
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
                    runningMode = {};
                    setStatus(Status::error, lastError.isNotEmpty() ? lastError : sonobus::video::translated(u8"H.264 编码进程已退出"));
                    hasPublisher = false;
                    nextCameraAttemptMs = nowMs + retryDelayMs;
                    retryDelayMs = juce::jmin(retryDelayMs * 2, 30000);
                }
                if ((! hasPublisher || desiredChanged) && nowMs >= nextCameraAttemptMs)
                {
                    stopPublisher();
                    lastAttemptRevision = desired.revision;
                    setStatus(Status::startingCamera);
                    auto mode = selectedCamera == runningDesired.cameraDeviceId && runningMode.isValid()
                                  ? runningMode
                                  : findPreferredCameraMode(ffmpegPath, selectedCamera);
                    if (! mode.isValid())
                    {
                        setStatus(Status::cameraUnavailable, lastError.isNotEmpty() ? lastError
                            : sonobus::video::translated(u8"摄像头没有可用的当前共享模式"));
                        nextCameraAttemptMs = nowMs + retryDelayMs;
                        retryDelayMs = juce::jmin(retryDelayMs * 2, 30000);
                    }
                    else
                    {
                        auto launchDesired = desired;
                        launchDesired.cameraDeviceId = selectedCamera;
                        if (startPublisher(ffmpegPath, devices, launchDesired, mode))
                        {
                            runningDesired = launchDesired;
                            runningMode = mode;
                            nextCameraAttemptMs = 0.0;
#if ! JUCE_WINDOWS
                            setStatus(Status::online);
#endif
                        }
                        else
                        {
                            runningMode = {};
                            setStatus(Status::cameraUnavailable, lastError.isNotEmpty() ? lastError
                                : sonobus::video::translated(u8"无法启动 H.264 硬件编码"));
                            nextCameraAttemptMs = nowMs + retryDelayMs;
                            retryDelayMs = juce::jmin(retryDelayMs * 2, 30000);
                        }
                    }
                }
                readPublisherProgress();
                {
                    const juce::ScopedLock lock(stateLock);
                    const auto minimumFps = juce::jmax(1.0, captureMode.fps - 1.0);
                    if (captureFps >= minimumFps) retryDelayMs = 1000;
                }
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

bool VideoRelayClient::requestEnrollment(int& pollAfterMs)
{
    juce::String localHost, localGroup, localUser, localClientId;
    juce::MemoryBlock localKey;
    EnrollmentHandler handler;
    {
        const juce::ScopedLock lock(stateLock);
        localHost = host;
        localGroup = group;
        localUser = user;
        localClientId = clientId;
        localKey = enrollmentKey;
        handler = enrollmentHandler;
    }
    if (localKey.getSize() != 32) return false;

    juce::MemoryBlock nonceBytes(18, true);
    auto& random = juce::Random::getSystemRandom();
    for (size_t i = 0; i < nonceBytes.getSize(); ++i)
        static_cast<juce::uint8*>(nonceBytes.getData())[i] = static_cast<juce::uint8>(random.nextInt(256));
    const auto nonce = toBase64Url(nonceBytes.getData(), nonceBytes.getSize());
    const auto timestamp = juce::Time::currentTimeMillis();
    const auto signatureInput = "enroll\n" + localGroup + "\n" + localUser + "\n" + localClientId + "\n"
                              + juce::String(timestamp) + "\n" + nonce;

    auto request = std::make_unique<juce::DynamicObject>();
    request->setProperty("group", localGroup);
    request->setProperty("user", localUser);
    request->setProperty("clientId", localClientId);
    request->setProperty("timestamp", static_cast<juce::int64>(timestamp));
    request->setProperty("nonce", nonce);
    request->setProperty("signature", hmacSha256Base64Url(localKey, signatureInput));
    const auto body = juce::JSON::toString(juce::var(request.release()), true);

    int statusCode = 0;
    juce::StringPairArray responseHeaders;
    auto endpoint = juce::URL("http://" + formatHost(localHost) + ":" + juce::String(controlPort) + "/video/control/enroll").withPOSTData(body);
    auto stream = endpoint.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs(5000)
                                                 .withExtraHeaders("Content-Type: application/json\r\n")
                                                 .withResponseHeaders(&responseHeaders)
                                                 .withStatusCode(&statusCode)
                                                 .withNumRedirectsToFollow(0));
    if (stream == nullptr || statusCode != 200)
    {
        const juce::ScopedLock lock(stateLock);
        lastError = statusCode > 0 ? sonobus::video::translated(u8"自动授权请求被拒绝，HTTP ") + juce::String(statusCode)
                                   : sonobus::video::translated(u8"无法连接摄像头授权服务");
        return false;
    }

    const auto response = juce::JSON::parse(stream->readEntireStreamAsString());
    auto* object = response.getDynamicObject();
    if (object == nullptr) return false;
    const auto payload = object->getProperty("payload").toString();
    const auto signature = object->getProperty("signature").toString();
    if (! secureEquals(signature, hmacSha256Base64Url(localKey, "enrollment-response\n" + nonce + "\n" + payload)))
    {
        const juce::ScopedLock lock(stateLock);
        lastError = sonobus::video::translated(u8"摄像头授权响应签名无效");
        return false;
    }
    pollAfterMs = juce::jlimit(250, 5000, static_cast<int>(object->getProperty("pollAfterMs")));

    juce::MemoryBlock decoded;
    if (! fromBase64Url(payload, decoded)) return false;
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(decoded.getData()), static_cast<int>(decoded.getSize())));
    auto* enrollment = parsed.getDynamicObject();
    if (enrollment == nullptr || enrollment->getProperty("type").toString() != "enrollment") return false;
    if (enrollment->getProperty("state").toString() != "approved")
    {
        setStatus(Status::awaitingAuthorization);
        return true;
    }

    const auto approvedPairingId = enrollment->getProperty("pairingId").toString().trim();
    juce::String saveError;
    if (approvedPairingId.isEmpty() || !handler || !handler(approvedPairingId, localKey, saveError))
    {
        setStatus(Status::error, saveError.isNotEmpty() ? saveError
            : sonobus::video::translated(u8"无法保存摄像头授权"));
        return false;
    }
    {
        const juce::ScopedLock lock(stateLock);
        pairingId = approvedPairingId;
        pairingKey = localKey;
        sequence = 0;
        pairingRejected = false;
        lastError.clear();
    }
    setStatus(Status::connecting);
    return true;
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
        lastError = statusCode > 0 ? sonobus::video::translated(u8"控制服务器拒绝请求，HTTP ") + juce::String(statusCode)
                                   : sonobus::video::translated(u8"无法连接摄像头控制服务器");
        pairingRejected = statusCode == 403;
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
        lastError = sonobus::video::translated(u8"控制服务器响应签名无效");
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
        lastError = sonobus::video::translated(u8"配对群组与当前 SonoBus 群组不一致");
        return false;
    }

    desired.enabled = static_cast<bool>(desiredObject->getProperty("enabled"));
    desired.cameraDeviceId = desiredObject->getProperty("cameraDeviceId").toString();
    desired.ingestPath = desiredObject->getProperty("ingestPath").toString();
    desired.publishUser = desiredObject->getProperty("publishUser").toString();
    desired.publishNonce = desiredObject->getProperty("publishNonce").toString();
    desired.rtspPort = juce::jlimit(1, 65535, static_cast<int>(desiredObject->getProperty("rtspPort")));
    desired.maxHeight = juce::jlimit(0, 4320, static_cast<int>(desiredObject->getProperty("maxHeight")));
    desired.maxFps = juce::jlimit(0.0, 240.0, static_cast<double>(desiredObject->getProperty("maxFps")));
    desired.maxBitrate = juce::jlimit(0, 100000000, static_cast<int>(desiredObject->getProperty("maxBitrate")));
    desired.revision = desiredObject->getProperty("revision").toString();
    pollAfterMs = juce::jlimit(250, 5000, static_cast<int>(object->getProperty("pollAfterMs")));
    {
        const juce::ScopedLock lock(stateLock);
        // Preserve camera/helper failures until the next successful capture state clears them.
        pairingRejected = false;
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
    result->setProperty("capturing", getStatus() == Status::online && publisher != nullptr && publisher->isRunning());
    result->setProperty("cameraDeviceId", activeCameraId);
    result->setProperty("cameraName", activeCamera);
    result->setProperty("codec", activeEncoder);
    if (activeMode.isValid())
    {
        result->setProperty("width", activeMode.width);
        result->setProperty("height", activeMode.height);
        if (actualFps > 0.0) result->setProperty("fps", actualFps);
    }
    if (captureMode.isValid())
    {
        result->setProperty("captureWidth", captureMode.width);
        result->setProperty("captureHeight", captureMode.height);
        result->setProperty("captureFps", captureFps > 0.0 ? captureFps : captureMode.fps);
    }
    if (actualBitrate > 0) result->setProperty("bitrate", actualBitrate);
    if (lastError.isNotEmpty()) result->setProperty("error", lastError);
    else if (cameraError.isNotEmpty()) result->setProperty("error", cameraError);
    return juce::var(result.release());
}

juce::Array<VideoRelayClient::CameraDevice> VideoRelayClient::getCameraDevices(const juce::String& ffmpegPath,
                                                                                juce::String& error) const
{
    juce::Array<CameraDevice> result;
#if JUCE_WINDOWS
    juce::ignoreUnused(ffmpegPath);
    const auto helper = findWindowsCaptureHelper();
    if (helper.isEmpty())
    {
        error = sonobus::video::translated(u8"安装包中缺少 Windows 共享摄像头运行时");
        return result;
    }
    juce::ChildProcess probe;
    if (! probe.start({ helper, "--list" }, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = sonobus::video::translated(u8"无法启动 Windows 共享摄像头枚举");
        return result;
    }
    juce::String output;
    if (! runProbe(probe, 10000, output))
        error = sonobus::video::translated(u8"Windows 共享摄像头枚举超时");
    for (const auto& device : sonobus::video::parseWindowsCameraDevices(output))
        result.add({ device.id, device.name });
    if (result.isEmpty() && error.isEmpty())
    {
        const auto failure = cameraFailureMessage(output);
        error = failure.isNotEmpty() ? failure : sonobus::video::translated(u8"未检测到摄像头");
    }
    // DirectShow devices: physical UVC cameras again, plus virtual cameras from OBS, YY etc.
    // that MediaFoundation never exposes. Virtual cameras can only be captured via DirectShow.
    {
        juce::ChildProcess dshow;
        if (dshow.start({ ffmpegPath, "-hide_banner", "-f", "dshow", "-list_devices", "true", "-i", "dummy" },
                        juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            juce::String dshowOutput;
            if (runProbe(dshow, 10000, dshowOutput))
            {
                // ffmpeg 7.x prints device lines without a section header, e.g.
                //   [dshow @ ...] "YY开播" (video)
                //   [dshow @ ...]   Alternative name "@device_sw_..."
                for (const auto& line : juce::StringArray::fromLines(dshowOutput))
                {
                    if (line.contains("Alternative name")) continue;
                    if (line.contains("(audio)")) continue;
                    const auto openQuote = line.indexOfChar('"');
                    if (openQuote < 0) continue;
                    const auto closeQuote = line.indexOfChar('"', openQuote + 1);
                    if (closeQuote < 0) continue;
                    const auto name = line.substring(openQuote + 1, closeQuote).trim();
                    if (name.isEmpty()) continue;
                    if (name.containsIgnoreCase("audio") || name.containsIgnoreCase("microphone")) continue;
                    // Skip names already listed by the MediaFoundation helper.
                    bool alreadyListed = false;
                    for (const auto& existing : result)
                        if (existing.name == name) { alreadyListed = true; break; }
                    if (! alreadyListed)
                        result.add({ "dshow:" + name, name });
                }
            }
        }
    }
#elif JUCE_MAC
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", "" };
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = sonobus::video::translated(u8"无法启动摄像头枚举程序");
        return result;
    }
    juce::String output;
    if (! runProbe(probe, 5000, output))
        error = sonobus::video::translated(u8"摄像头枚举超时");
    bool inVideoSection = false;
    for (const auto& line : juce::StringArray::fromLines(output))
    {
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
    }
    if (result.isEmpty() && error.isEmpty()) error = sonobus::video::translated(u8"未检测到摄像头");
#else
    juce::ignoreUnused(ffmpegPath, error);
#endif
    return result;
}

juce::Array<VideoRelayClient::CameraMode> VideoRelayClient::getPreferredCameraModes(const juce::String& ffmpegPath,
                                                                                 const juce::String& cameraDeviceId,
                                                                                 juce::String& error) const
{
    juce::Array<CameraMode> result;
#if JUCE_WINDOWS
    juce::ignoreUnused(ffmpegPath);
    const auto helper = findWindowsCaptureHelper();
    juce::ChildProcess probe;
    if (helper.isEmpty() || ! probe.start({ helper, "--modes", "--device", cameraDeviceId },
                                           juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = sonobus::video::translated(u8"无法读取 Windows 共享摄像头模式");
        return result;
    }
    juce::String outputText;
    if (! runProbe(probe, 15000, outputText))
        error = sonobus::video::translated(u8"读取 Windows 共享摄像头模式超时");
    for (const auto& mode : sonobus::video::parseWindowsCameraModes(outputText))
        result.addIfNotAlreadyThere({ mode.width, mode.height, mode.fps });
    if (result.isEmpty() && error.isEmpty())
    {
        const auto failure = cameraFailureMessage(outputText);
        error = failure.isNotEmpty() ? failure : sonobus::video::translated(u8"摄像头没有可共享的当前模式");
    }
#elif JUCE_MAC
    const juce::StringArray arguments { ffmpegPath, "-hide_banner", "-loglevel", "info", "-f", "avfoundation",
                                         "-framerate", juce::String(targetFps), "-video_size", "1x1",
                                         "-i", cameraDeviceId + ":none", "-frames:v", "1", "-f", "null", "-" };
    const std::regex modePattern(R"((\d+)x(\d+)@\[\s*([0-9.]+)\s+([0-9.]+)\]fps)");
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = sonobus::video::translated(u8"无法读取摄像头模式");
        return result;
    }
    juce::String outputText;
    if (! runProbe(probe, 5000, outputText))
        error = sonobus::video::translated(u8"读取摄像头模式超时");
    const auto output = outputText.toStdString();
    for (auto match = std::sregex_iterator(output.begin(), output.end(), modePattern); match != std::sregex_iterator(); ++match)
    {
        if (std::stod((*match)[4].str()) < 59.0) continue;
        result.addIfNotAlreadyThere({ std::stoi((*match)[1].str()), std::stoi((*match)[2].str()), targetFps });
    }
    if (result.isEmpty() && error.isEmpty())
        error = sonobus::video::translated(u8"摄像头没有真实的 60 FPS 模式");
#else
    juce::ignoreUnused(ffmpegPath, cameraDeviceId, error);
#endif
    std::sort(result.begin(), result.end(), [](const CameraMode& left, const CameraMode& right)
    {
        const auto leftFast = left.fps >= 59.0;
        const auto rightFast = right.fps >= 59.0;
        if (leftFast != rightFast) return leftFast;
        const auto leftPixels = static_cast<juce::int64>(left.width) * left.height;
        const auto rightPixels = static_cast<juce::int64>(right.width) * right.height;
        return leftPixels == rightPixels ? left.fps > right.fps : leftPixels > rightPixels;
    });
    return result;
}

VideoRelayClient::CameraMode VideoRelayClient::findPreferredCameraMode(const juce::String& ffmpegPath,
                                                                    const juce::String& cameraDeviceId)
{
#if JUCE_WINDOWS
    // The helper selects the actual current SharedReadOnly source once, inside --publish.
    juce::ignoreUnused(ffmpegPath, cameraDeviceId);
    return { 1280, 720, 30.0 };
#else
    juce::String modeError;
    const auto modes = getPreferredCameraModes(ffmpegPath, cameraDeviceId, modeError);
    for (const auto mode : modes)
    {
        if (threadShouldExit()) break;
        juce::String probeError;
        if (probeCameraMode(ffmpegPath, cameraDeviceId, mode, probeError)) return mode;
        if (modeError.isEmpty()) modeError = probeError;
        if (sonobus::video::classifyCameraFailure(probeError) == sonobus::video::CameraFailure::busy) break;
    }
    if (modeError.isNotEmpty())
    {
        const juce::ScopedLock lock(stateLock);
        lastError = modeError;
    }
    return {};
#endif
}

bool VideoRelayClient::probeCameraMode(const juce::String& ffmpegPath,
                                       const juce::String& cameraDeviceId,
                                       CameraMode mode,
                                       juce::String& error) const
{
#if JUCE_WINDOWS
    juce::ignoreUnused(ffmpegPath, cameraDeviceId, error);
    return mode.isValid();
#else
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "error" };
    arguments.addArray(captureArguments(cameraDeviceId, mode));
    arguments.addArray({ "-frames:v", "30", "-an", "-f", "null", "-" });
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = sonobus::video::translated(u8"无法启动摄像头探测");
        return false;
    }
    juce::String output;
    const auto finished = runProbe(probe, 4000, output);
    if (! finished)
    {
        error = sonobus::video::translated(u8"摄像头响应超时");
        return false;
    }
    if (! threadShouldExit() && probe.getExitCode() == 0) return true;
    error = cameraFailureMessage(output);
    if (error.isEmpty()) error = sonobus::video::translated(u8"摄像头模式探测失败");
    return false;
#endif
}

bool VideoRelayClient::probeEncoder(const juce::String& ffmpegPath,
                                    const juce::String& cameraDeviceId,
                                    CameraMode mode,
                                    const juce::String& encoder,
                                    juce::String& error) const
{
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "error" };
#if JUCE_WINDOWS
    juce::ignoreUnused(cameraDeviceId);
    arguments.addArray({ "-f", "lavfi", "-i", "color=c=black:s=" + juce::String(mode.width) + "x"
                                                + juce::String(mode.height) + ":r=" + juce::String(mode.fps, 3) });
#else
    arguments.addArray(captureArguments(cameraDeviceId, mode));
#endif
    const auto gop = juce::jmax(1, juce::roundToInt(mode.fps));
    // One synthetic frame validates encoder setup; 30 high-resolution frames can time out on software H.264.
    arguments.addArray({ "-frames:v", "1", "-an", "-fps_mode", "passthrough", "-c:v", encoder });
    arguments.addArray(encoderArguments(encoder));
    arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0",
                         "-g", juce::String(gop), "-f", "null", "-" });
    juce::ChildProcess probe;
    if (! probe.start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
    juce::String output;
    const auto finished = runProbe(probe, 2000, output);
    if (! finished)
    {
        error = sonobus::video::translated(u8"H.264 编码器探测超时");
        return false;
    }
    if (! threadShouldExit() && probe.getExitCode() == 0) return true;
    error = cameraFailureMessage(output);
    if (error.isEmpty()) error = sonobus::video::lastOutputLine(output);
    if (error.isEmpty()) error = sonobus::video::translated(u8"H.264 编码器探测失败");
    return false;
}

VideoRelayClient::CameraMode VideoRelayClient::outputModeFor(CameraMode capture, const DesiredState& desired)
{
    const auto output = sonobus::video::constrainOutputMode(
        { capture.width, capture.height, capture.fps }, desired.maxHeight, desired.maxFps);
    return { output.width, output.height, output.fps };
}

bool VideoRelayClient::startPublisher(const juce::String& ffmpegPath,
                                      const juce::Array<CameraDevice>& devices,
                                      const DesiredState& desired,
                                      CameraMode mode)
{
    const auto outputMode = outputModeFor(mode, desired);
    const auto encoders = availableEncoders(ffmpegPath);
#if JUCE_WINDOWS
    const auto encoderProbeMode = outputMode;
    juce::ignoreUnused(encoderProbeMode);
#else
    const auto encoderProbeMode = mode;
#endif
#if JUCE_MAC
    const juce::StringArray preferred { "h264_videotoolbox", "libx264" };
#elif JUCE_WINDOWS
    const juce::StringArray preferred { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libx264" };
#else
    const juce::StringArray preferred { "libx264" };
#endif
    juce::String launchErrors;
    // Virtual DirectShow cameras (OBS/YY etc.) are not MediaCapture devices at all;
    // they bypass the helper entirely and go straight to the DirectShow fallback below.
    const auto dshowOnly = desired.cameraDeviceId.startsWith("dshow:");
    for (const auto& encoder : preferred)
    {
        if (threadShouldExit()) break;
        if (dshowOnly) break;
        if (! encoders.contains(encoder)) continue;
#if JUCE_WINDOWS
        // Skip synthetic lavfi probes on Windows: antivirus startup scans can blow a 2s budget,
        // stalling the control loop (client looks offline). The 1.5s publisher startup check
        // below already falls through to the next encoder on failure; libx264 is built-in.
#else
        juce::String probeError;
        if (! probeEncoder(ffmpegPath, desired.cameraDeviceId, encoderProbeMode, encoder, probeError))
        {
            if (launchErrors.isEmpty()) launchErrors = probeError;
            continue;
        }
#endif
        auto process = std::make_unique<juce::ChildProcess>();
        const auto arguments = publisherArguments(ffmpegPath, desired.cameraDeviceId, mode, encoder, desired);
        if (! process->start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) continue;
        if (process->waitForProcessToFinish(1500))
        {
            launchErrors = cameraFailureMessage(process->readAllProcessOutput());
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
#if JUCE_WINDOWS
            captureMode = {};
#else
            captureMode = mode;
#endif
            activeMaxHeight = desired.maxHeight;
            activeMaxFps = desired.maxFps;
            activeMaxBitrate = desired.maxBitrate;
            activeMode = outputMode;
            captureFps = 0.0;
            actualFps = 0.0;
            actualBitrate = 0;
            progressBuffer.clear();
            lastError.clear();
        }
        return true;
    }
    {
        const juce::ScopedLock lock(stateLock);
        lastError = launchErrors.isNotEmpty() ? launchErrors
            : sonobus::video::translated(u8"没有可用的 H.264 编码器或编码硬件");
    }
#if JUCE_WINDOWS
    // Last resort: some USB cameras cannot initialise MediaCapture/FrameReader at all
    // (both exclusive and shared attempts fail with E_FAIL). DirectShow works for every UVC
    // camera, so fall back to ffmpeg dshow capture with libx264.
    {
        juce::String dshowName;
        for (const auto& device : devices)
            if (device.id == desired.cameraDeviceId) dshowName = device.name;
        if (dshowName.isNotEmpty())
        {
            auto process = std::make_unique<juce::ChildProcess>();
            auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "warning", "-nostdin",
                                                 "-f", "dshow", "-i", dshowName };
            arguments.add("-an");
            arguments.addArray({ "-fps_mode", "passthrough", "-c:v", "libx264" });
            arguments.addArray(encoderArguments("libx264"));
            const auto automaticBitrate = bitrateFor(mode.width, mode.height);
            const auto bitrate = desired.maxBitrate > 0 ? juce::jmin(desired.maxBitrate, automaticBitrate) : automaticBitrate;
            const auto gop = juce::jmax(1, juce::roundToInt(mode.fps));
            arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0", "-g", juce::String(gop),
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
            if (process->start(arguments, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)
                && ! process->waitForProcessToFinish(1500))
            {
                auto cameraName = dshowName;
                {
                    const juce::ScopedLock lock(stateLock);
                    publisher = std::move(process);
                    activeCameraId = desired.cameraDeviceId;
                    activeCamera = cameraName;
                    activeEncoder = "libx264";
                    captureMode = {};
                    activeMaxHeight = desired.maxHeight;
                    activeMaxFps = desired.maxFps;
                    activeMaxBitrate = desired.maxBitrate;
                    activeMode = outputMode;
                    captureFps = 0.0;
                    actualFps = 0.0;
                    actualBitrate = 0;
                    progressBuffer.clear();
                    lastError.clear();
                }
                return true;
            }
            launchErrors = cameraFailureMessage(process->readAllProcessOutput());
        }
    }
#endif
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
    captureMode = {};
    captureFps = 0.0;
    actualFps = 0.0;
    actualBitrate = 0;
    activeMaxHeight = 0;
    activeMaxFps = 0.0;
    activeMaxBitrate = 0;
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
        if (line.startsWith("capture_fps="))
        {
            captureFps = line.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
            if (captureFps > 0.5)
            {
                captureMode.fps = captureFps;
                lastError.clear();
                status.store(Status::online);
            }
        }
        else if (line.startsWith("fps="))
        {
            actualFps = line.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
        }
        if (line.startsWith("capture_width=")) captureMode.width = line.fromFirstOccurrenceOf("=", false, false).getIntValue();
        if (line.startsWith("capture_height=")) captureMode.height = line.fromFirstOccurrenceOf("=", false, false).getIntValue();
        if (line.startsWith("capture_nominal_fps=")) captureMode.fps = line.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
        if (line.startsWith("bitrate="))
        {
            const auto value = line.fromFirstOccurrenceOf("=", false, false).retainCharacters("0123456789.");
            if (value.isNotEmpty()) actualBitrate = juce::roundToInt(value.getDoubleValue() * 1000.0);
        }
        if (line.startsWith("SONOBUS_ERROR=")) lastError = cameraFailureMessage(line);
        else if (line.startsWith("source_candidate=") || line.startsWith("capture_source="))
            lastError = line;  // keep camera-source diagnostics visible in the admin UI
        else if (! line.containsChar('=') && line.isNotEmpty()) lastError = line.substring(0, 500);
    }
    if (captureMode.isValid())
    {
        DesiredState activeDesired;
        activeDesired.maxHeight = activeMaxHeight;
        activeDesired.maxFps = activeMaxFps;
        activeDesired.maxBitrate = activeMaxBitrate;
        activeMode = outputModeFor(captureMode, activeDesired);
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

juce::String VideoRelayClient::findWindowsCaptureHelper() const
{
#if JUCE_WINDOWS
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("SONOBUS_VIDEO_CAPTURE_HELPER_PATH", {});
    if (overridePath.isNotEmpty() && juce::File(overridePath).existsAsFile()) return overridePath;
    const auto sibling = moduleFile().getSiblingFile("SonoBusVideoCaptureHelper.exe");
    if (sibling.existsAsFile()) return sibling.getFullPathName();
#endif
    return {};
}

juce::StringArray VideoRelayClient::availableEncoders(const juce::String& ffmpegPath) const
{
    juce::ChildProcess probe;
    if (! probe.start({ ffmpegPath, "-hide_banner", "-encoders" }, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return {};

    juce::String output;
    runProbe(probe, 5000, output);
    juce::StringArray result;
    for (const auto& name : { "h264_videotoolbox", "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libx264" })
        if (output.containsWholeWord(name)) result.add(name);
    return result;
}

juce::StringArray VideoRelayClient::captureArguments(const juce::String& cameraDeviceId, CameraMode mode) const
{
#if JUCE_MAC
    return { "-f", "avfoundation", "-framerate", juce::String(mode.fps, 3),
             "-video_size", juce::String(mode.width) + "x" + juce::String(mode.height),
             "-use_wallclock_as_timestamps", "1", "-i", cameraDeviceId + ":none" };
#elif JUCE_WINDOWS
    juce::ignoreUnused(cameraDeviceId, mode);
    return {};
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
#if JUCE_WINDOWS
    auto arguments = juce::StringArray { findWindowsCaptureHelper(), "--publish", "--device", cameraDeviceId,
                                         "--ffmpeg", ffmpegPath, "--max-height", juce::String(desired.maxHeight),
                                         "--max-fps", juce::String(desired.maxFps, 3), "--max-bitrate",
                                         juce::String(desired.maxBitrate), "--parent-pid",
                                         juce::String(static_cast<juce::int64>(GetCurrentProcessId())), "--" };
    arguments.addArray({ "-an", "-vf", "@SONOBUS_FILTER@", "-fps_mode", "passthrough", "-c:v", encoder });
    arguments.addArray(encoderArguments(encoder));
    arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0", "-g", "@SONOBUS_GOP@",
                         "-b:v", "@SONOBUS_BITRATE@", "-maxrate", "@SONOBUS_BITRATE@",
                         "-bufsize", "@SONOBUS_BUFSIZE@", "-progress", "pipe:1", "-nostats" });
    juce::ignoreUnused(mode);
#else
    auto arguments = juce::StringArray { ffmpegPath, "-hide_banner", "-loglevel", "warning", "-nostdin" };
    arguments.addArray(captureArguments(cameraDeviceId, mode));
    const auto outputMode = outputModeFor(mode, desired);
    juce::StringArray filters;
    if (outputMode.width != mode.width || outputMode.height != mode.height)
        filters.add("scale=" + juce::String(outputMode.width) + ":" + juce::String(outputMode.height) + ":flags=fast_bilinear");
    if (outputMode.fps + 0.01 < mode.fps)
        filters.add("select=isnan(prev_selected_t)+gte(t-prev_selected_t\\,"
                    + juce::String(1.0 / outputMode.fps, 6) + ")");
    const auto gop = juce::jmax(1, juce::roundToInt(outputMode.fps));
    arguments.add("-an");
    if (! filters.isEmpty()) arguments.addArray({ "-vf", filters.joinIntoString(",") });
    arguments.addArray({ "-fps_mode", "passthrough", "-c:v", encoder });
    arguments.addArray(encoderArguments(encoder));
    const auto automaticBitrate = bitrateFor(outputMode.width, outputMode.height);
    const auto bitrate = desired.maxBitrate > 0 ? juce::jmin(desired.maxBitrate, automaticBitrate) : automaticBitrate;
    arguments.addArray({ "-pix_fmt", "yuv420p", "-profile:v", "baseline", "-bf", "0", "-g", juce::String(gop),
                         "-b:v", juce::String(bitrate), "-maxrate", juce::String(bitrate),
                         "-bufsize", juce::String(bitrate / 2), "-progress", "pipe:1", "-nostats" });
#endif

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
    else if (newStatus == Status::online || newStatus == Status::waitingForAdmin
             || newStatus == Status::awaitingAuthorization || newStatus == Status::connecting)
        lastError.clear();
}

juce::String VideoRelayClient::getStatusText() const
{
    const juce::ScopedTryLock lock(stateLock);
    if (! lock.isLocked())
        return sonobus::video::translated(u8"状态同步中…");
    const auto separator = sonobus::video::utf8(u8" · ");
    switch (getStatus())
    {
        case Status::unpaired:          return sonobus::video::translated(u8"等待管理员授权") + (lastError.isNotEmpty() ? separator + lastError : "");
        case Status::awaitingAuthorization: return sonobus::video::translated(u8"等待后台管理员授权；无需输入授权码");
        case Status::connecting:        return sonobus::video::translated(u8"连接管理员控制服务中");
        case Status::waitingForAdmin:   return sonobus::video::translated(u8"已连接，等待管理员开启摄像头");
        case Status::startingCamera:    return sonobus::video::translated(u8"读取当前共享最高模式并启动 H.264 编码");
        case Status::online:
        {
            auto text = sonobus::video::translated(u8"H.264 视频在线");
            if (activeMode.isValid()) text += separator + juce::String(activeMode.width) + sonobus::video::utf8(u8"×") + juce::String(activeMode.height);
            if (actualFps > 0.0) text += separator + juce::String(actualFps, 1) + " FPS";
            if (actualBitrate > 0) text += separator + juce::String(actualBitrate / 1000000.0, 1) + " Mbps";
            return text;
        }
        case Status::cameraUnavailable: return sonobus::video::translated(u8"摄像头不可用") + (lastError.isNotEmpty() ? separator + lastError : "");
        case Status::error:             return sonobus::video::translated(u8"视频错误") + (lastError.isNotEmpty() ? separator + lastError : "");
        case Status::idle:              break;
    }
    return sonobus::video::translated(u8"未连接");
}

juce::String VideoRelayClient::getActiveCamera() const
{
    const juce::ScopedTryLock lock(stateLock);
    if (! lock.isLocked()) return {};
    return activeCamera;
}


juce::MemoryBlock VideoRelayClient::deriveEnrollmentKey(const juce::MemoryBlock& secret,
                                                        const juce::String& group,
                                                        const juce::String& user,
                                                        const juce::String& clientId)
{
    juce::MemoryOutputStream material;
    static constexpr char prefix[] = "sonobus-video-enrollment-v1\0";
    material.write(prefix, sizeof(prefix) - 1);
    material.write(secret.getData(), secret.getSize());
    material.writeByte(0);
    const auto groupUtf8 = group.toUTF8();
    material.write(groupUtf8.getAddress(), group.getNumBytesAsUTF8());
    material.writeByte(0);
    const auto userUtf8 = user.toUTF8();
    material.write(userUtf8.getAddress(), user.getNumBytesAsUTF8());
    material.writeByte(0);
    const auto clientUtf8 = clientId.toUTF8();
    material.write(clientUtf8.getAddress(), clientId.getNumBytesAsUTF8());
    return juce::SHA256(material.getMemoryBlock()).getRawData();
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
