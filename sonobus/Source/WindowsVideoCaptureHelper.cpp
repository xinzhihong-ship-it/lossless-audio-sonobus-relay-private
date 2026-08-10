// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mferror.h>
#include <winrt/base.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Capture;
using namespace Windows::Media::Capture::Frames;
using namespace Windows::Media::MediaProperties;

struct __declspec(uuid("5B0D3235-4DBA-4D44-8654-1D7689D2E1F0")) IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace
{
struct Mode
{
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    MediaFrameFormat format { nullptr };
};

struct FrameState
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> frame;
    uint64_t sequence = 0;
    bool failed = false;
    HRESULT failureCode = S_OK;
};

struct CaptureSession
{
    MediaCapture capture { nullptr };
    MediaFrameSource source { nullptr };
    MediaFrameReader reader { nullptr };
    event_token frameToken {};
    event_token failedToken {};
    Mode mode;
    std::shared_ptr<FrameState> state;
};

std::string cleanField(std::string value)
{
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

std::wstring option(const std::vector<std::wstring>& args, const wchar_t* name)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return {};
}

uint32_t numberOption(const std::vector<std::wstring>& args, const wchar_t* name, uint32_t fallback)
{
    const auto value = option(args, name);
    if (value.empty()) return fallback;
    try { return static_cast<uint32_t>(std::stoul(value)); }
    catch (...) { return fallback; }
}

double doubleOption(const std::vector<std::wstring>& args, const wchar_t* name, double fallback)
{
    const auto value = option(args, name);
    if (value.empty()) return fallback;
    try { return std::stod(value); }
    catch (...) { return fallback; }
}

double frameRate(const MediaFrameFormat& format)
{
    const auto ratio = format.FrameRate();
    return ratio.Denominator() == 0 ? 0.0 : static_cast<double>(ratio.Numerator()) / ratio.Denominator();
}

MediaFrameSourceGroup findGroup(const hstring& groupId)
{
    for (const auto& group : MediaFrameSourceGroup::FindAllAsync().get())
        if (group.Id() == groupId) return group;
    return nullptr;
}

Mode currentMode(const MediaFrameSource& source)
{
    const auto format = source.CurrentFormat();
    const auto video = format ? format.VideoFormat() : nullptr;
    return video ? Mode { video.Width(), video.Height(), frameRate(format), format } : Mode {};
}

bool isPreferredMode(const Mode& candidate, const Mode& current)
{
    const auto candidateRate = static_cast<double>(candidate.width) * candidate.height * candidate.fps;
    const auto currentRate = static_cast<double>(current.width) * current.height * current.fps;
    if (candidateRate != currentRate) return candidateRate > currentRate;
    const auto candidatePixels = static_cast<uint64_t>(candidate.width) * candidate.height;
    const auto currentPixels = static_cast<uint64_t>(current.width) * current.height;
    return candidatePixels != currentPixels ? candidatePixels > currentPixels : candidate.fps > current.fps;
}

CaptureSession openSharedCamera(const hstring& deviceId, uint32_t requestedWidth = 0, uint32_t requestedHeight = 0,
                                double requestedFps = 0.0)
{
    const auto group = findGroup(deviceId);
    if (!group) throw hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"The selected camera source group is unavailable.");

    MediaCaptureInitializationSettings settings;
    settings.SourceGroup(group);
    settings.StreamingCaptureMode(StreamingCaptureMode::Video);
    settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);
    settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);

    CaptureSession session;
    session.capture = MediaCapture();
    session.capture.InitializeAsync(settings).get();
    for (const auto& entry : session.capture.FrameSources())
    {
        const auto source = entry.Value();
        if (source.Info().SourceKind() != MediaFrameSourceKind::Color) continue;
        const auto mode = currentMode(source);
        if (!session.source || isPreferredMode(mode, session.mode))
        {
            session.source = source;
            session.mode = mode;
        }
    }
    if (!session.source) throw hresult_error(E_FAIL, L"No color camera source was exposed.");
    if (!session.mode.width || !session.mode.height || session.mode.fps <= 0.0)
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"The current shared camera stream has no valid frame rate.");
    // SharedReadOnly formats can change between enumeration and publish startup; use the actual current mode.
    (void) requestedWidth;
    (void) requestedHeight;
    (void) requestedFps;
    return session;
}

bool copyNv12(const SoftwareBitmap& input, std::vector<uint8_t>& output)
{
    if (!input) return false;
    auto bitmap = input;
    if (bitmap.BitmapPixelFormat() != BitmapPixelFormat::Nv12)
        bitmap = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Nv12);
    if (!bitmap || bitmap.BitmapPixelFormat() != BitmapPixelFormat::Nv12) return false;

    auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
    auto reference = buffer.CreateReference();
    auto access = reference.as<IMemoryBufferByteAccess>();
    uint8_t* bytes = nullptr;
    uint32_t capacity = 0;
    check_hresult(access->GetBuffer(&bytes, &capacity));
    if (!bytes || buffer.GetPlaneCount() < 2) return false;

    const auto width = static_cast<size_t>(bitmap.PixelWidth());
    const auto height = static_cast<size_t>(bitmap.PixelHeight());
    output.resize(width * height * 3 / 2);
    size_t destination = 0;
    for (uint32_t planeIndex = 0; planeIndex < 2; ++planeIndex)
    {
        const auto plane = buffer.GetPlaneDescription(planeIndex);
        const auto rows = planeIndex == 0 ? height : height / 2;
        for (size_t row = 0; row < rows; ++row)
        {
            const auto sourceIndex = static_cast<size_t>(plane.StartIndex) + row * static_cast<size_t>(plane.Stride);
            if (sourceIndex + width > capacity || destination + width > output.size()) return false;
            std::copy_n(bytes + sourceIndex, width, output.data() + destination);
            destination += width;
        }
    }
    return destination == output.size();
}

void emitError(HRESULT code)
{
    std::string category = "unavailable";
    if (code == E_ACCESSDENIED || code == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) category = "permission";
    else if (code == HRESULT_FROM_WIN32(ERROR_BUSY) || code == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
             || code == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) category = "busy";
    std::cout << "SONOBUS_ERROR=" << category << ":0x" << std::hex << static_cast<uint32_t>(code) << std::dec << std::endl;
}

CaptureSession startReader(const hstring& deviceId, uint32_t width, uint32_t height, double fps)
{
    auto session = openSharedCamera(deviceId, width, height, fps);
    session.state = std::make_shared<FrameState>();
    session.failedToken = session.capture.Failed([state = session.state](const MediaCapture&, const MediaCaptureFailedEventArgs& args)
    {
        std::lock_guard lock(state->mutex);
        state->failed = true;
        state->failureCode = args.Code();
        state->changed.notify_all();
    });

    try
    {
        session.reader = session.capture.CreateFrameReaderAsync(session.source, MediaEncodingSubtypes::Nv12()).get();
    }
    catch (const hresult_error&)
    {
        session.reader = session.capture.CreateFrameReaderAsync(session.source, MediaEncodingSubtypes::Bgra8()).get();
    }
    session.reader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Realtime);
    session.frameToken = session.reader.FrameArrived([state = session.state](const MediaFrameReader& reader, const MediaFrameArrivedEventArgs&)
    {
        try
        {
            const auto frame = reader.TryAcquireLatestFrame();
            if (!frame) return;
            const auto video = frame.VideoMediaFrame();
            if (!video) return;
            std::vector<uint8_t> pixels;
            if (!copyNv12(video.SoftwareBitmap(), pixels)) return;
            {
                std::lock_guard lock(state->mutex);
                state->frame = std::move(pixels);
                ++state->sequence;
            }
            state->changed.notify_one();
        }
        catch (const hresult_error& error)
        {
            std::lock_guard lock(state->mutex);
            state->failed = true;
            state->failureCode = error.code();
            state->changed.notify_all();
        }
    });
    if (session.reader.StartAsync().get() != MediaFrameReaderStartStatus::Success)
        throw hresult_error(E_FAIL, L"The shared camera frame reader could not start.");
    return session;
}

std::wstring quoteArgument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : value)
    {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct ChildProcess
{
    PROCESS_INFORMATION process {};
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE job = nullptr;

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept
        : process(std::exchange(other.process, {})),
          input(std::exchange(other.input, INVALID_HANDLE_VALUE)),
          job(std::exchange(other.job, nullptr))
    {
    }
    ChildProcess& operator=(ChildProcess&& other) noexcept
    {
        if (this != &other)
        {
            close();
            process = std::exchange(other.process, {});
            input = std::exchange(other.input, INVALID_HANDLE_VALUE);
            job = std::exchange(other.job, nullptr);
        }
        return *this;
    }
    ~ChildProcess() { close(); }

private:
    void close() noexcept
    {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(input, INVALID_HANDLE_VALUE));
        if (process.hThread) CloseHandle(std::exchange(process.hThread, nullptr));
        if (process.hProcess) CloseHandle(std::exchange(process.hProcess, nullptr));
        if (job) CloseHandle(std::exchange(job, nullptr));
    }
};

ChildProcess startFfmpeg(const std::wstring& ffmpeg, const std::vector<std::wstring>& outputArguments,
                         uint32_t width, uint32_t height, double fps)
{
    SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE inputRead = INVALID_HANDLE_VALUE;
    HANDLE inputWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&inputRead, &inputWrite, &security, 0)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    if (!SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0))
    {
        const auto error = GetLastError();
        CloseHandle(inputRead);
        CloseHandle(inputWrite);
        throw hresult_error(HRESULT_FROM_WIN32(error));
    }

    std::vector<std::wstring> arguments {
        ffmpeg, L"-hide_banner", L"-loglevel", L"warning", L"-nostdin",
        L"-f", L"rawvideo", L"-pixel_format", L"nv12", L"-video_size",
        std::to_wstring(width) + L"x" + std::to_wstring(height),
        L"-framerate", std::to_wstring(fps), L"-use_wallclock_as_timestamps", L"1", L"-i", L"pipe:0"
    };
    arguments.insert(arguments.end(), outputArguments.begin(), outputArguments.end());
    std::wstring command;
    for (const auto& argument : arguments)
    {
        if (!command.empty()) command.push_back(L' ');
        command += quoteArgument(argument);
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inputRead;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    ChildProcess child;
    child.input = inputWrite;
    child.job = CreateJobObjectW(nullptr, nullptr);
    if (!child.job)
    {
        const auto error = GetLastError();
        CloseHandle(inputRead);
        throw hresult_error(HRESULT_FROM_WIN32(error));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        const auto error = GetLastError();
        CloseHandle(inputRead);
        throw hresult_error(HRESULT_FROM_WIN32(error));
    }

    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(0);
    if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.process))
    {
        const auto error = GetLastError();
        CloseHandle(inputRead);
        throw hresult_error(HRESULT_FROM_WIN32(error));
    }
    CloseHandle(inputRead);
    if (!AssignProcessToJobObject(child.job, child.process.hProcess))
    {
        const auto error = GetLastError();
        TerminateProcess(child.process.hProcess, 1);
        WaitForSingleObject(child.process.hProcess, 5000);
        throw hresult_error(HRESULT_FROM_WIN32(error));
    }
    return child;
}

int listCameras()
{
    for (const auto& group : MediaFrameSourceGroup::FindAllAsync().get())
    {
        bool hasColor = false;
        for (const auto& info : group.SourceInfos())
            if (info.SourceKind() == MediaFrameSourceKind::Color) { hasColor = true; break; }
        if (hasColor)
            std::cout << "SONOBUS_CAMERA\t" << cleanField(to_string(group.Id())) << '\t' << cleanField(to_string(group.DisplayName())) << '\n';
    }
    return 0;
}

int listModes(const hstring& deviceId)
{
    const auto session = openSharedCamera(deviceId);
    const auto& mode = session.mode;
    std::cout << "SONOBUS_MODE\t" << mode.width << '\t' << mode.height << '\t' << mode.fps << '\n';
    return 0;
}

int publish(const std::vector<std::wstring>& args)
{
    const auto device = option(args, L"--device");
    const auto ffmpeg = option(args, L"--ffmpeg");
    const auto width = numberOption(args, L"--width", 0);
    const auto height = numberOption(args, L"--height", 0);
    const auto fps = doubleOption(args, L"--fps", 0.0);
    const auto separator = std::find(args.begin(), args.end(), L"--");
    if (device.empty() || ffmpeg.empty() || !width || !height || fps <= 0.0 || separator == args.end()) return 2;
    std::vector<std::wstring> outputArguments(separator + 1, args.end());

    auto camera = startReader(hstring(device), width, height, fps);
    auto child = startFfmpeg(ffmpeg, outputArguments, camera.mode.width, camera.mode.height, camera.mode.fps);
    std::cout << "capture_width=" << camera.mode.width << "\ncapture_height=" << camera.mode.height
              << "\ncapture_nominal_fps=" << camera.mode.fps << "\n" << std::flush;

    uint64_t lastSequence = 0;
    uint64_t frames = 0;
    auto fpsWindow = std::chrono::steady_clock::now();
    while (WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT)
    {
        std::vector<uint8_t> frame;
        {
            std::unique_lock lock(camera.state->mutex);
            camera.state->changed.wait_for(lock, std::chrono::seconds(5), [&]
            {
                return camera.state->failed || camera.state->sequence != lastSequence;
            });
            if (camera.state->failed)
            {
                emitError(camera.state->failureCode);
                return 3;
            }
            if (camera.state->sequence == lastSequence)
            {
                std::cout << "SONOBUS_ERROR=unavailable:frame-timeout" << std::endl;
                return 3;
            }
            lastSequence = camera.state->sequence;
            frame = camera.state->frame;
        }

        DWORD written = 0;
        size_t offset = 0;
        while (offset < frame.size())
        {
            const auto chunk = static_cast<DWORD>(std::min<size_t>(frame.size() - offset, 1024 * 1024));
            if (!WriteFile(child.input, frame.data() + offset, chunk, &written, nullptr) || written == 0) return 4;
            offset += written;
        }
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - fpsWindow).count();
        if (elapsed >= 3.0)
        {
            const auto measuredFps = frames / elapsed;
            std::cout << "capture_fps=" << measuredFps << '\n' << std::flush;
            const auto minimumFps = std::max(1.0, camera.mode.fps - 1.0);
            if (measuredFps < minimumFps)
            {
                std::cout << "SONOBUS_ERROR=unavailable:shared-frame-rate-below-expected" << std::endl;
                return 3;
            }
            frames = 0;
            fpsWindow = now;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(child.process.hProcess, &exitCode);
    return static_cast<int>(exitCode);
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        init_apartment(apartment_type::multi_threaded);
        std::vector<std::wstring> args(argv + 1, argv + argc);
        if (std::find(args.begin(), args.end(), L"--list") != args.end()) return listCameras();
        if (std::find(args.begin(), args.end(), L"--modes") != args.end())
        {
            const auto device = option(args, L"--device");
            return device.empty() ? 2 : listModes(hstring(device));
        }
        if (std::find(args.begin(), args.end(), L"--publish") != args.end()) return publish(args);
        return 2;
    }
    catch (const hresult_error& error)
    {
        emitError(error.code());
        return 3;
    }
    catch (...)
    {
        std::cout << "SONOBUS_ERROR=unavailable:unexpected" << std::endl;
        return 3;
    }
}

#endif
