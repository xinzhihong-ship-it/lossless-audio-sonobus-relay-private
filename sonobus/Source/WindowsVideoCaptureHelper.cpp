// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mferror.h>
#include "../../tools/windows-molixiu-bridge/SonoBusMoLiXiuFrame.h"
#include <winrt/base.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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
using namespace Windows::Storage::Streams;


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
    uint64_t frameArrivals = 0;
    uint64_t emptyFrames = 0;
    std::string failureStage;
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
    std::vector<MediaFrameSource> sources;
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
        session.sources.push_back(source);
    }
    if (session.sources.empty()) throw hresult_error(E_FAIL, L"No color camera source was exposed.");
    // Try the highest-throughput source first, but keep the others as fallbacks: on some
    // cameras only one of several color sources actually produces frames.
    std::sort(session.sources.begin(), session.sources.end(), [](const MediaFrameSource& left, const MediaFrameSource& right)
    {
        return isPreferredMode(currentMode(left), currentMode(right));
    });
    for (const auto& source : session.sources)
    {
        const auto mode = currentMode(source);
        std::cout << "source_candidate=" << mode.width << "x" << mode.height << "@" << mode.fps << '\n';
    }
    std::cout << std::flush;
    // Formats can change between enumeration and publish startup; use the actual current mode.
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

    const auto width = static_cast<size_t>(bitmap.PixelWidth());
    const auto height = static_cast<size_t>(bitmap.PixelHeight());
    const auto size = width * height * 3 / 2;
    if (!width || !height || size > UINT32_MAX) return false;
    Buffer raw(static_cast<uint32_t>(size));
    raw.Length(static_cast<uint32_t>(size));
    bitmap.CopyToBuffer(raw);
    output.resize(size);
    auto reader = DataReader::FromBuffer(raw);
    reader.ReadBytes(winrt::array_view<uint8_t>(output));
    return true;
}

void emitError(HRESULT code, const std::string& stage = {})
{
    std::string category = "unavailable";
    if (code == E_ACCESSDENIED || code == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) category = "permission";
    else if (code == HRESULT_FROM_WIN32(ERROR_BUSY) || code == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
             || code == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) category = "busy";
    std::cout << "SONOBUS_ERROR=" << category;
    if (!stage.empty()) std::cout << ":" << stage;
    std::cout << ":0x" << std::hex << static_cast<uint32_t>(code) << std::dec << std::endl;
}

CaptureSession startReaderForSource(CaptureSession& session, size_t sourceIndex)
{
    if (sourceIndex >= session.sources.size()) throw hresult_error(E_FAIL, L"No usable camera frame source.");
    session.source = session.sources[sourceIndex];
    session.mode = currentMode(session.source);
    if (!session.mode.width || !session.mode.height || session.mode.fps <= 0.0)
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"The current camera stream has no valid frame rate.");
    session.state = std::make_shared<FrameState>();
    session.failedToken = session.capture.Failed([state = session.state](const MediaCapture&, const MediaCaptureFailedEventArgs& args)
    {
        std::lock_guard lock(state->mutex);
        state->failed = true;
        state->failureCode = args.Code();
        state->failureStage = "capture";
        state->changed.notify_all();
    });
    const auto onFrame = [state = session.state](const MediaFrameReader& reader, const MediaFrameArrivedEventArgs&)
    {
        try
        {
            const auto frame = reader.TryAcquireLatestFrame();
            {
                std::lock_guard lock(state->mutex);
                ++state->frameArrivals;
                if (!frame) ++state->emptyFrames;
            }
            if (!frame) return;
            const auto video = frame.VideoMediaFrame();
            if (!video)
            {
                std::lock_guard lock(state->mutex);
                state->failed = true;
                state->failureCode = E_FAIL;
                state->failureStage = "no-video-frame";
                state->changed.notify_all();
                return;
            }
            const auto bitmap = video.SoftwareBitmap();
            if (!bitmap)
            {
                std::lock_guard lock(state->mutex);
                state->failed = true;
                state->failureCode = E_FAIL;
                state->failureStage = "no-software-bitmap";
                state->changed.notify_all();
                return;
            }
            std::vector<uint8_t> pixels;
            if (!copyNv12(bitmap, pixels))
            {
                std::lock_guard lock(state->mutex);
                state->failed = true;
                state->failureCode = E_FAIL;
                state->failureStage = "frame-copy";
                state->changed.notify_all();
                return;
            }
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
            state->failureStage = "frame";
            state->changed.notify_all();
        }
    };
    const auto startReader = [&session, &onFrame](const hstring& subtype)
    {
        try
        {
            session.reader = subtype.empty()
                           ? session.capture.CreateFrameReaderAsync(session.source).get()
                           : session.capture.CreateFrameReaderAsync(session.source, subtype).get();
        }
        catch (const hresult_error&)
        {
            session.reader = nullptr;
            return false;
        }
        session.reader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Realtime);
        session.frameToken = session.reader.FrameArrived(onFrame);
        MediaFrameReaderStartStatus status;
        try
        {
            status = session.reader.StartAsync().get();
        }
        catch (const hresult_error& error)
        {
            std::cout << "reader_start_error=0x" << std::hex << static_cast<uint32_t>(error.code())
                      << std::dec << '\n' << std::flush;
            session.reader.FrameArrived(session.frameToken);
            session.reader.Close();
            session.reader = nullptr;
            return false;
        }
        if (status == MediaFrameReaderStartStatus::Success) return true;
        std::cout << "reader_start_status=" << static_cast<int>(status) << '\n' << std::flush;
        session.reader.FrameArrived(session.frameToken);
        session.reader.Close();
        session.reader = nullptr;
        return false;
    };
    // Some camera drivers accept the default reader but deliver empty media frames.
    // Request concrete pixel formats so a successful reader also carries video data.
    if (! startReader(MediaEncodingSubtypes::Bgra8())
        && ! startReader(MediaEncodingSubtypes::Argb32())
        && ! startReader(MediaEncodingSubtypes::Nv12())
        && ! startReader(MediaEncodingSubtypes::Yuy2()))
    {
        std::cout << "SONOBUS_ERROR=unavailable:reader-start" << std::endl;
        throw hresult_error(E_FAIL, L"The shared camera frame reader could not start.");
    }
    return session;
}

void stopReader(CaptureSession& session)
{
    if (session.reader)
    {
        session.reader.FrameArrived(session.frameToken);
        session.reader.Close();
        session.reader = nullptr;
    }
    session.source = nullptr;
    session.state.reset();
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

Mode outputModeFor(Mode capture, uint32_t maxHeight, double maxFps)
{
    if (maxHeight > 0 && maxHeight < capture.height)
    {
        const auto sourceWidth = capture.width;
        const auto sourceHeight = capture.height;
        capture.height = maxHeight - (maxHeight % 2);
        if (capture.height < 2) capture.height = 2;
        const auto scaledWidth = static_cast<uint64_t>(sourceWidth) * capture.height / sourceHeight;
        capture.width = static_cast<uint32_t>((scaledWidth / 2) * 2);
        if (capture.width < 2) capture.width = 2;
    }
    if (maxFps > 0.0 && maxFps < capture.fps) capture.fps = maxFps;
    capture.format = nullptr;
    return capture;
}

uint32_t automaticBitrate(uint32_t width, uint32_t height)
{
    const auto pixels = static_cast<uint64_t>(width) * height;
    if (pixels >= 3840ULL * 2160ULL) return 20000000;
    if (pixels >= 2560ULL * 1440ULL) return 12000000;
    if (pixels >= 1920ULL * 1080ULL) return 8000000;
    if (pixels >= 1280ULL * 720ULL) return 5000000;
    return 3000000;
}

std::wstring outputFilter(Mode capture, Mode output)
{
    std::wstring filter;
    if (output.width != capture.width || output.height != capture.height)
        filter = L"scale=" + std::to_wstring(output.width) + L":" + std::to_wstring(output.height) + L":flags=fast_bilinear";
    if (output.fps + 0.01 < capture.fps)
    {
        if (!filter.empty()) filter += L",";
        filter += L"select=isnan(prev_selected_t)+gte(t-prev_selected_t\\,"
                + std::to_wstring(1.0 / output.fps) + L")";
    }
    return filter;
}

std::vector<std::wstring> expandOutputArguments(const std::vector<std::wstring>& input, Mode capture,
                                                uint32_t maxHeight, double maxFps, uint32_t maxBitrate)
{
    const auto output = outputModeFor(capture, maxHeight, maxFps);
    const auto automatic = automaticBitrate(output.width, output.height);
    const auto bitrate = maxBitrate > 0 ? std::min(maxBitrate, automatic) : automatic;
    const auto filter = outputFilter(capture, output);
    const auto gop = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(output.fps)));
    std::vector<std::wstring> result;
    for (const auto& argument : input)
    {
        if (argument == L"@SONOBUS_FILTER@")
        {
            if (filter.empty())
            {
                if (!result.empty() && result.back() == L"-vf") result.pop_back();
            }
            else result.push_back(filter);
        }
        else if (argument == L"@SONOBUS_GOP@") result.push_back(std::to_wstring(gop));
        else if (argument == L"@SONOBUS_BITRATE@") result.push_back(std::to_wstring(bitrate));
        else if (argument == L"@SONOBUS_BUFSIZE@") result.push_back(std::to_wstring(bitrate / 2));
        else result.push_back(argument);
    }
    return result;
}

const wchar_t* ffmpegPixelFormat(uint32_t pixelFormat)
{
    switch (pixelFormat)
    {
        case sonobus::molixiu::kPixelBgr24: return L"bgr24";
        case sonobus::molixiu::kPixelBgra32: return L"bgra";
        case sonobus::molixiu::kPixelYuy2: return L"yuyv422";
        case sonobus::molixiu::kPixelNv12: return L"nv12";
        default: return nullptr;
    }
}

ChildProcess spawnChildProcess(const std::vector<std::wstring>& arguments);

ChildProcess startFfmpeg(const std::wstring& ffmpeg, const std::vector<std::wstring>& outputArguments,
                         uint32_t width, uint32_t height, double fps, uint32_t maxHeight,
                         double maxFps, uint32_t maxBitrate, uint32_t pixelFormat)
{
    const auto inputPixelFormat = ffmpegPixelFormat(pixelFormat);
    if (inputPixelFormat == nullptr)
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"Unsupported shared video pixel format.");
    const auto capture = Mode { width, height, fps, nullptr };
    const auto expandedArguments = expandOutputArguments(outputArguments, capture, maxHeight, maxFps, maxBitrate);

    std::vector<std::wstring> arguments {
        ffmpeg, L"-hide_banner", L"-loglevel", L"warning", L"-nostdin",
        L"-f", L"rawvideo", L"-pixel_format", inputPixelFormat, L"-video_size",
        std::to_wstring(width) + L"x" + std::to_wstring(height),
        L"-framerate", std::to_wstring(fps), L"-use_wallclock_as_timestamps", L"1", L"-i", L"pipe:0"
    };
    arguments.insert(arguments.end(), expandedArguments.begin(), expandedArguments.end());
    return spawnChildProcess(arguments);
}

ChildProcess spawnChildProcess(const std::vector<std::wstring>& arguments)
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
    if (!CreateProcessW(arguments.front().c_str(), writable.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.process))
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

std::vector<std::wstring> expandDshowArguments(const std::vector<std::wstring>& input,
                                               uint32_t maxHeight, double maxFps, uint32_t maxBitrate)
{
    // The dshow input size is unknown up front, so cap the height while keeping
    // the source aspect ratio instead of the exact rawvideo scale.
    std::wstring filter;
    if (maxHeight > 0) filter = L"scale=-2:'min(ih," + std::to_wstring(maxHeight) + L")'";
    if (maxFps > 0.0)
    {
        if (! filter.empty()) filter += L",";
        filter += L"fps=" + std::to_wstring(maxFps);
    }
    const auto nominal = automaticBitrate(1920, 1080);
    const auto bitrate = maxBitrate > 0 ? std::min(maxBitrate, nominal) : nominal;
    const auto gop = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(maxFps > 0.0 ? maxFps : 30.0)));
    std::vector<std::wstring> result;
    for (const auto& argument : input)
    {
        if (argument == L"@SONOBUS_FILTER@")
        {
            if (filter.empty())
            {
                if (!result.empty() && result.back() == L"-vf") result.pop_back();
            }
            else result.push_back(filter);
        }
        else if (argument == L"@SONOBUS_GOP@") result.push_back(std::to_wstring(gop));
        else if (argument == L"@SONOBUS_BITRATE@") result.push_back(std::to_wstring(bitrate));
        else if (argument == L"@SONOBUS_BUFSIZE@") result.push_back(std::to_wstring(bitrate / 2));
        else result.push_back(argument);
    }
    return result;
}

ChildProcess startFfmpegDshow(const std::wstring& ffmpeg, const std::wstring& device,
                              const std::vector<std::wstring>& outputArguments,
                              uint32_t maxHeight, double maxFps, uint32_t maxBitrate)
{
    // quiet: a busy/gone device must not surface "already in use"-style lines that
    // the main app classifies as a busy failure and reacts to by releasing us.
    std::vector<std::wstring> arguments {
        ffmpeg, L"-hide_banner", L"-loglevel", L"quiet", L"-nostdin",
        L"-f", L"dshow", L"-i", L"video=" + device
    };
    const auto expandedArguments = expandDshowArguments(outputArguments, maxHeight, maxFps, maxBitrate);
    arguments.insert(arguments.end(), expandedArguments.begin(), expandedArguments.end());
    return spawnChildProcess(arguments);
}

std::string runCommandCapture(const std::wstring& command)
{
    SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (! CreatePipe(&readPipe, &writePipe, &security, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process {};
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(0);
    if (! CreateProcessW(nullptr, writable.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return {};
    }
    CloseHandle(writePipe);
    std::string output;
    char buffer[4096];
    DWORD bytes = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0)
        output.append(buffer, bytes);
    WaitForSingleObject(process.hProcess, 10000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readPipe);
    return output;
}

int listCameras(const std::vector<std::wstring>& args)
{
    const auto ffmpeg = option(args, L"--ffmpeg");
    std::vector<std::string> listedNames;
    for (const auto& group : MediaFrameSourceGroup::FindAllAsync().get())
    {
        bool hasColor = false;
        for (const auto& info : group.SourceInfos())
            if (info.SourceKind() == MediaFrameSourceKind::Color) { hasColor = true; break; }
        if (hasColor)
        {
            const auto name = cleanField(to_string(group.DisplayName()));
            std::cout << "SONOBUS_CAMERA\t" << cleanField(to_string(group.Id())) << '\t' << name << '\n';
            listedNames.push_back(name);
        }
    }
    // Virtual cameras (OBS/YY etc.) exist only as DirectShow devices; ffmpeg's dshow
    // listing reaches them. Run it from inside the helper so one trusted process
    // (already allowed by security software) performs every enumeration.
    if (! ffmpeg.empty())
    {
        const auto output = runCommandCapture(quoteArgument(ffmpeg) + L" -hide_banner -f dshow -list_devices true -i dummy");
        std::istringstream lines(output);
        std::string line;
        std::vector<std::pair<std::string, std::string>> dshowDevices;  // name, alternative name
        bool lastWasAudio = false;
        while (std::getline(lines, line))
        {
            const auto altMarker = line.find("Alternative name");
            if (altMarker != std::string::npos)
            {
                // An alternative name line always belongs to the device listed just before it.
                // Audio devices (skipped above) must not overwrite the previous video device.
                if (! lastWasAudio && ! dshowDevices.empty())
                {
                    const auto openQuote = line.find('"', altMarker);
                    const auto closeQuote = openQuote == std::string::npos ? std::string::npos : line.find('"', openQuote + 1);
                    if (openQuote != std::string::npos && closeQuote != std::string::npos)
                        dshowDevices.back().second = line.substr(openQuote + 1, closeQuote - openQuote - 1);
                }
                continue;
            }
            if (line.find("(audio)") != std::string::npos) { lastWasAudio = true; continue; }
            const auto openQuote = line.find('"');
            if (openQuote == std::string::npos) continue;
            const auto closeQuote = line.find('"', openQuote + 1);
            if (closeQuote == std::string::npos) continue;
            auto name = line.substr(openQuote + 1, closeQuote - openQuote - 1);
            if (name.empty()) continue;
            auto lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("audio") != std::string::npos || lower.find("microphone") != std::string::npos) { lastWasAudio = true; continue; }
            dshowDevices.emplace_back(cleanField(name), std::string());
            lastWasAudio = false;
        }
        for (const auto& [name, alternative] : dshowDevices)
        {
            bool duplicate = false;
            for (const auto& existing : listedNames)
                if (existing == name) { duplicate = true; break; }
            if (duplicate) continue;
            // Prefer the ASCII alternative name (@device_sw_...) as the device id so the
            // capture step never has to match Chinese/localised display names.
            const auto deviceId = alternative.empty() ? name : alternative;
            std::cout << "SONOBUS_CAMERA\tdshow:" << cleanField(deviceId) << '\t' << name << '\n';
            listedNames.push_back(name);
        }
    }
    return 0;
}

int listModes(const hstring& deviceId)
{
    const auto session = openSharedCamera(deviceId);
    for (const auto& source : session.sources)
    {
        const auto mode = currentMode(source);
        std::cout << "SONOBUS_MODE\t" << mode.width << '\t' << mode.height << '\t' << mode.fps << '\n';
    }
    return 0;
}
void pumpStaCallbacks()
{
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DWORD result = 0;
    CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, 10, 0, nullptr, &result);
}

int publish(const std::vector<std::wstring>& args)
{
    const auto device = option(args, L"--device");
    const auto ffmpeg = option(args, L"--ffmpeg");
    const auto width = numberOption(args, L"--width", 0);
    const auto height = numberOption(args, L"--height", 0);
    const auto fps = doubleOption(args, L"--fps", 0.0);
    const auto maxHeight = numberOption(args, L"--max-height", 0);
    const auto maxFps = doubleOption(args, L"--max-fps", 0.0);
    const auto maxBitrate = numberOption(args, L"--max-bitrate", 0);
    const auto parentPid = numberOption(args, L"--parent-pid", 0);
    const auto separator = std::find(args.begin(), args.end(), L"--");
    if (device.empty() || ffmpeg.empty() || separator == args.end()) return 2;
    std::vector<std::wstring> outputArguments(separator + 1, args.end());

    // Exit promptly when the SonoBus process that spawned us has gone away.
    HANDLE parentHandle = nullptr;
    if (parentPid != 0)
        parentHandle = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    // Physical cameras must never block OBS/直播伴侣 from opening the same device.
    CaptureSession camera;
    size_t sourceIndex = 0;
    try
    {
        camera = openSharedCamera(hstring(device), width, height, fps);
    }
    catch (const hresult_error& error)
    {
        std::cout << "SONOBUS_ERROR=unavailable:shared:0x" << std::hex << static_cast<uint32_t>(error.code())
                  << std::dec << std::endl;
        return 3;
    }
    startReaderForSource(camera, sourceIndex);
    auto child = startFfmpeg(ffmpeg, outputArguments, camera.mode.width, camera.mode.height, camera.mode.fps,
                             maxHeight, maxFps, maxBitrate, sonobus::molixiu::kPixelNv12);
    std::cout << "capture_mode=shared\n"
              << "capture_source=" << sourceIndex << '\n'
              << "capture_width=" << camera.mode.width << "\ncapture_height=" << camera.mode.height
              << "\ncapture_nominal_fps=" << camera.mode.fps << "\n" << std::flush;

    uint64_t lastSequence = 0;
    uint64_t frames = 0;
    auto fpsWindow = std::chrono::steady_clock::now();
    while (WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT)
    {
        if (parentHandle != nullptr && WaitForSingleObject(parentHandle, 0) == WAIT_OBJECT_0) break;
        std::vector<uint8_t> frame;
        {
            // MediaCapture was initialized on an STA thread (required by Windows), so its
            // FrameArrived callbacks arrive via COM/window messages and must be pumped while
            // we wait; a plain condition-variable wait would starve them.
            const auto waitSeconds = lastSequence == 0 ? 30 : 5;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);
            for (;;)
            {
                pumpStaCallbacks();
                std::lock_guard lock(camera.state->mutex);
                if (camera.state->failed || camera.state->sequence != lastSequence) break;
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
            if (camera.state->failed)
            {
                emitError(camera.state->failureCode, camera.state->failureStage);
                return 3;
            }
            if (camera.state->sequence == lastSequence)
            {
                // No frames yet: if this is the first frame and more color sources remain,
                // some cameras expose multiple color sources where only one produces frames.
                bool switchedSource = false;
                if (lastSequence == 0 && sourceIndex + 1 < camera.sources.size())
                {
                    do
                    {
                        stopReader(camera);
                        ++sourceIndex;
                        try
                        {
                            startReaderForSource(camera, sourceIndex);
                            switchedSource = true;
                        }
                        catch (...)
                        {
                            // Source unusable; keep trying the remaining candidates.
                        }
                    } while (! switchedSource && sourceIndex + 1 < camera.sources.size());
                    if (switchedSource)
                    {
                        std::cout << "capture_source=" << sourceIndex << '\n' << std::flush;
                        continue;
                    }
                }
                const auto stage = ! camera.state ? "no-source"
                                  : camera.state->frameArrivals == 0 ? "no-frame-arrival"
                                  : camera.state->emptyFrames > 0 ? "empty-frame" : "no-frame-progress";
                std::cout << "SONOBUS_ERROR=unavailable:frame-timeout:" << stage << std::endl;
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
            frames = 0;
            fpsWindow = now;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(child.process.hProcess, &exitCode);
    return static_cast<int>(exitCode);
}

struct SharedMoLiXiuFrame
{
    sonobus::molixiu::FrameHeader header {};
    std::vector<uint8_t> pixels;
};

LONG readSharedSequence(const sonobus::molixiu::FrameHeader* mapped) noexcept
{
    if (mapped == nullptr) return 0;
    __try
    {
        MemoryBarrier();
        const auto sequence = *reinterpret_cast<const volatile LONG*>(&mapped->sequence);
        MemoryBarrier();
        return sequence;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool readSharedMoLiXiuFrame(const sonobus::molixiu::FrameHeader* mapped, SharedMoLiXiuFrame& output)
{
    if (mapped == nullptr) return false;
    // The helper maps this section read-only. InterlockedCompareExchange is a
    // read-modify-write operation and faults on that mapping even when the
    // compared value is unchanged.
    const auto sequence = readSharedSequence(mapped);
    if (sequence <= 0 || (sequence & 1) != 0) return false;

    sonobus::molixiu::FrameHeader header {};
    std::memcpy(&header, mapped, sizeof(header));
    if (header.magic != sonobus::molixiu::kFrameMagic
        || header.version != sonobus::molixiu::kFrameVersion
        || header.width < 2 || header.height < 2
        || header.width > 8192 || header.height > 8192
        || header.bytes == 0 || header.bytes > sonobus::molixiu::kMaxFrameBytes)
        return false;
    const auto pixels = static_cast<uint64_t>(header.width) * header.height;
    const uint64_t expected = header.pixelFormat == sonobus::molixiu::kPixelBgr24 ? pixels * 3
                            : header.pixelFormat == sonobus::molixiu::kPixelBgra32 ? pixels * 4
                            : header.pixelFormat == sonobus::molixiu::kPixelYuy2 ? pixels * 2
                            : header.pixelFormat == sonobus::molixiu::kPixelNv12 ? pixels * 3 / 2 : 0;
    if (expected == 0 || expected != header.bytes) return false;

    std::vector<uint8_t> frame(header.bytes);
    std::memcpy(frame.data(), reinterpret_cast<const uint8_t*>(mapped) + sizeof(header), header.bytes);
    MemoryBarrier();
    const auto sequenceAfter = readSharedSequence(mapped);
    if (sequenceAfter != sequence || (sequenceAfter & 1) != 0) return false;
    output.header = header;
    output.header.sequence = static_cast<uint32_t>(sequenceAfter);
    output.pixels = std::move(frame);
    return true;
}

struct MoLiXiuCameraHint
{
    DWORD pid = 0;
    std::wstring device;
};

bool readMoLiXiuCameraHint(MoLiXiuCameraHint& output)
{
    wchar_t appData[32768] {};
    const auto appDataLength = GetEnvironmentVariableW(
        L"APPDATA", appData, static_cast<DWORD>(sizeof(appData) / sizeof(appData[0])));
    if (appDataLength == 0 || appDataLength >= sizeof(appData) / sizeof(appData[0])) return false;
    const auto path = std::wstring(appData, appDataLength) + L"\\SonoBus\\molixiu-camera.txt";
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string utf8;
    char buffer[4096];
    DWORD bytes = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0)
        utf8.append(buffer, bytes);
    CloseHandle(file);

    std::istringstream lines(utf8);
    std::string line;
    bool deviceFound = false;
    while (std::getline(lines, line))
    {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("pid=", 0) == 0)
        {
            try { output.pid = static_cast<DWORD>(std::stoul(line.substr(4))); }
            catch (...) { output.pid = 0; }
        }
        else if (line.rfind("device=", 0) == 0)
        {
            const auto value = line.substr(7);
            const auto wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                        static_cast<int>(value.size()), nullptr, 0);
            if (wideLength > 0)
            {
                std::wstring wide(static_cast<size_t>(wideLength), L'\0');
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), wide.data(), wideLength) == wideLength)
                {
                    output.device = std::move(wide);
                    deviceFound = true;
                }
            }
        }
    }
    return deviceFound;
}

hstring findSourceGroupByDisplayName(const std::wstring& name)
{
    for (const auto& group : MediaFrameSourceGroup::FindAllAsync().get())
    {
        const auto displayName = std::wstring(group.DisplayName().c_str());
        if (! displayName.empty() && _wcsicmp(displayName.c_str(), name.c_str()) == 0) return group.Id();
    }
    return hstring {};
}

int publishMoLiXiu(const std::vector<std::wstring>& args)
{
    const auto ffmpeg = option(args, L"--ffmpeg");
    const auto maxHeight = numberOption(args, L"--max-height", 0);
    const auto maxFps = doubleOption(args, L"--max-fps", 0.0);
    const auto maxBitrate = numberOption(args, L"--max-bitrate", 0);
    const auto parentPid = numberOption(args, L"--parent-pid", 0);
    const auto separator = std::find(args.begin(), args.end(), L"--");
    if (ffmpeg.empty() || separator == args.end()) return 2;
    const std::vector<std::wstring> outputArguments(separator + 1, args.end());

    HANDLE parentHandle = parentPid == 0 ? nullptr : OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    HANDLE mapping = nullptr;
    auto* mapped = static_cast<sonobus::molixiu::FrameHeader*>(nullptr);

    // Single pipe encoder, fed either from MoLiXiu hook frames or, during the
    // fallback, from a shared MediaCapture reader on MoLiXiu's selected camera.
    ChildProcess child;
    uint32_t childWidth = 0;
    uint32_t childHeight = 0;
    double childFps = 0.0;
    uint32_t childPixel = 0;
    const auto childMatches = [&](uint32_t width, uint32_t height, double fps, uint32_t pixelFormat)
    {
        return child.process.hProcess != nullptr && childWidth == width && childHeight == height
            && childFps == fps && childPixel == pixelFormat;
    };
    const auto restartChild = [&](uint32_t width, uint32_t height, double fps, uint32_t pixelFormat)
    {
        child = ChildProcess {};
        child = startFfmpeg(ffmpeg, outputArguments, width, height, fps, maxHeight, maxFps, maxBitrate, pixelFormat);
        childWidth = width;
        childHeight = height;
        childFps = fps;
        childPixel = pixelFormat;
    };

    uint32_t lastSequence = 0;
    SharedMoLiXiuFrame frame;
    auto lastHookFrameAt = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    auto fpsWindow = std::chrono::steady_clock::now();

    // Direct fallback: when MoLiXiu stops feeding its internal callback (camera
    // closed, video-file playback, ...), keep streaming the camera it selected
    // (recorded by the bridge/hook in %APPDATA%\SonoBus\molixiu-camera.txt)
    // until hook frames resume. MediaCapture opens shared read-only; the dshow
    // encoder is held only in short windows so MoLiXiu can always reclaim it.
    enum class Fallback { None, SharedReader, DshowChild };
    Fallback fallback = Fallback::None;
    CaptureSession direct;
    uint64_t directSequence = 0;
    ChildProcess dshowChild;
    bool sawDeviceHint = false;
    auto fallbackRetryAt = std::chrono::steady_clock::now();
    auto dshowReleaseAt = std::chrono::steady_clock::time_point::max();
    auto dshowHeartbeatAt = std::chrono::steady_clock::time_point::min();

    const auto stopFallback = [&]()
    {
        if (fallback == Fallback::SharedReader)
        {
            stopReader(direct);
            direct = CaptureSession {};
            directSequence = 0;
        }
        else if (fallback == Fallback::DshowChild)
        {
            dshowChild = ChildProcess {};
        }
        fallback = Fallback::None;
    };

    for (;;)
    {
        if (parentHandle != nullptr && WaitForSingleObject(parentHandle, 0) == WAIT_OBJECT_0) break;
        const auto now = std::chrono::steady_clock::now();
        if (fallback == Fallback::None && ! sawDeviceHint
            && now - lastHookFrameAt > std::chrono::seconds(30))
        {
            std::cout << "SONOBUS_ERROR=unavailable:molixiu-frame-timeout" << std::endl;
            return 3;
        }
        if (mapping == nullptr)
        {
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sonobus::molixiu::kFrameMappingName);
            if (mapping != nullptr)
            {
                mapped = static_cast<sonobus::molixiu::FrameHeader*>(MapViewOfFile(
                    mapping, FILE_MAP_READ, 0, 0, sonobus::molixiu::kFrameMappingBytes));
                if (mapped == nullptr)
                {
                    CloseHandle(mapping);
                    mapping = nullptr;
                }
            }
        }

        SharedMoLiXiuFrame next;
        if (readSharedMoLiXiuFrame(mapped, next) && next.header.sequence != lastSequence)
        {
            // Hook frames take priority; their return ends any fallback.
            lastHookFrameAt = std::chrono::steady_clock::now();
            if (fallback != Fallback::None)
            {
                stopFallback();
                std::cout << "molixiu_source=hook" << std::endl;
            }
            const auto fpsValue = next.header.reserved > 0 ? next.header.reserved / 1000.0 : 30.0;
            if (! childMatches(next.header.width, next.header.height, fpsValue, next.header.pixelFormat))
            {
                try
                {
                    restartChild(next.header.width, next.header.height, fpsValue, next.header.pixelFormat);
                    std::cout << "capture_mode=molixiu-hook\n"
                              << "capture_width=" << next.header.width << "\ncapture_height=" << next.header.height
                              << "\ncapture_nominal_fps=" << fpsValue << '\n' << std::flush;
                }
                catch (const hresult_error& error)
                {
                    emitError(error.code(), "molixiu-ffmpeg");
                    return 3;
                }
            }
            frame = std::move(next);
            lastSequence = frame.header.sequence;
        }
        else if (fallback == Fallback::None
                 && (sawDeviceHint
                     || std::chrono::steady_clock::now() - lastHookFrameAt > std::chrono::seconds(5))
                 && std::chrono::steady_clock::now() >= fallbackRetryAt)
        {
            MoLiXiuCameraHint hint;
            bool started = false;
            if (readMoLiXiuCameraHint(hint))
            {
                sawDeviceHint = true;
                const auto groupId = findSourceGroupByDisplayName(hint.device);
                if (! groupId.empty())
                {
                    try
                    {
                        auto session = openSharedCamera(groupId);
                        startReaderForSource(session, 0);
                        restartChild(session.mode.width, session.mode.height, session.mode.fps,
                                     sonobus::molixiu::kPixelNv12);
                        direct = std::move(session);
                        fallback = Fallback::SharedReader;
                        directSequence = 0;
                        started = true;
                    }
                    catch (const hresult_error& error)
                    {
                        std::cout << "molixiu_direct_error=shared:0x" << std::hex
                                  << static_cast<uint32_t>(error.code()) << std::dec << std::endl;
                    }
                }
                if (! started)
                {
                    try
                    {
                        child = ChildProcess {};  // only one encoder may publish at a time
                        dshowChild = startFfmpegDshow(ffmpeg, hint.device, outputArguments,
                                                      maxHeight, maxFps, maxBitrate);
                        fallback = Fallback::DshowChild;
                        dshowReleaseAt = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                        dshowHeartbeatAt = std::chrono::steady_clock::now();
                        started = true;
                    }
                    catch (const hresult_error& error)
                    {
                        std::cout << "molixiu_direct_error=dshow:0x" << std::hex
                                  << static_cast<uint32_t>(error.code()) << std::dec << std::endl;
                    }
                }
                if (started)
                {
                    frame.pixels.clear();
                    std::cout << "capture_mode=molixiu-direct" << std::endl;
                }
            }
            if (! started)
                fallbackRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }

        if (child.process.hProcess && WaitForSingleObject(child.process.hProcess, 0) != WAIT_TIMEOUT)
        {
            DWORD exitCode = 1;
            GetExitCodeProcess(child.process.hProcess, &exitCode);
            return static_cast<int>(exitCode);
        }
        if (child.process.hProcess && ! frame.pixels.empty())
        {
            DWORD written = 0;
            size_t offset = 0;
            while (offset < frame.pixels.size())
            {
                const auto chunk = static_cast<DWORD>(std::min<size_t>(frame.pixels.size() - offset, 1024 * 1024));
                if (!WriteFile(child.input, frame.pixels.data() + offset, chunk, &written, nullptr) || written == 0)
                    return 4;
                offset += written;
            }
            frame.pixels.clear();
            ++frames;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - fpsWindow).count();
            if (elapsed >= 3.0)
            {
                std::cout << "capture_fps=" << frames / elapsed << '\n' << std::flush;
                frames = 0;
                fpsWindow = std::chrono::steady_clock::now();
            }
        }
        if (fallback == Fallback::SharedReader)
        {
            // MediaCapture was initialized on this STA thread, so its frame
            // callbacks only arrive while window messages are pumped.
            pumpStaCallbacks();
            bool failed = false;
            HRESULT failureCode = S_OK;
            {
                std::lock_guard lock(direct.state->mutex);
                failed = direct.state->failed;
                failureCode = direct.state->failureCode;
            }
            if (failed)
            {
                std::cout << "molixiu_direct_error=capture:0x" << std::hex
                          << static_cast<uint32_t>(failureCode) << std::dec << std::endl;
                stopFallback();
                child = ChildProcess {};
                fallbackRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            else if (direct.state->sequence != directSequence)
            {
                std::vector<uint8_t> pixels;
                {
                    std::lock_guard lock(direct.state->mutex);
                    pixels = direct.state->frame;
                    directSequence = direct.state->sequence;
                }
                if (child.process.hProcess && ! pixels.empty())
                {
                    DWORD written = 0;
                    size_t offset = 0;
                    while (offset < pixels.size())
                    {
                        const auto chunk = static_cast<DWORD>(std::min<size_t>(pixels.size() - offset, 1024 * 1024));
                        if (!WriteFile(child.input, pixels.data() + offset, chunk, &written, nullptr) || written == 0)
                            return 4;
                        offset += written;
                    }
                    ++frames;
                    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - fpsWindow).count();
                    if (elapsed >= 3.0)
                    {
                        std::cout << "capture_fps=" << frames / elapsed << '\n' << std::flush;
                        frames = 0;
                        fpsWindow = std::chrono::steady_clock::now();
                    }
                }
            }
        }
        else if (fallback == Fallback::DshowChild)
        {
            if (dshowChild.process.hProcess != nullptr
                && WaitForSingleObject(dshowChild.process.hProcess, 0) != WAIT_TIMEOUT)
            {
                // The dshow encoder exited (device busy or gone): retry shortly.
                dshowChild = ChildProcess {};
                fallback = Fallback::None;
                fallbackRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            else if (std::chrono::steady_clock::now() >= dshowReleaseAt)
            {
                // Release the device for a moment so MoLiXiu (or any other app)
                // can reclaim it; returning hook frames end the fallback first.
                dshowChild = ChildProcess {};
                fallback = Fallback::None;
                fallbackRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            }
            else if (dshowChild.process.hProcess != nullptr
                     && std::chrono::steady_clock::now() - dshowHeartbeatAt >= std::chrono::seconds(3))
            {
                // ponytail: nominal-fps heartbeat; the quiet dshow encoder hides
                // its real rate and the main app needs capture_fps to stay alive.
                std::cout << "capture_fps=" << (maxFps > 0.0 ? maxFps : 30.0) << '\n' << std::flush;
                dshowHeartbeatAt = std::chrono::steady_clock::now();
            }
        }
        Sleep(3);
    }
    if (mapped != nullptr) UnmapViewOfFile(mapped);
    if (mapping != nullptr) CloseHandle(mapping);
    if (parentHandle != nullptr) CloseHandle(parentHandle);
    return 1;
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        // MediaCapture.InitializeAsync must run on an STA thread; FrameArrived callbacks are
        // delivered through that apartment and are pumped in the publish loop.
        init_apartment(apartment_type::single_threaded);
        std::vector<std::wstring> args(argv + 1, argv + argc);
        if (std::find(args.begin(), args.end(), L"--list") != args.end()) return listCameras(args);
        if (std::find(args.begin(), args.end(), L"--modes") != args.end())
        {
            const auto device = option(args, L"--device");
            return device.empty() ? 2 : listModes(hstring(device));
        }
        if (std::find(args.begin(), args.end(), L"--publish-molixiu") != args.end()) return publishMoLiXiu(args);
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
