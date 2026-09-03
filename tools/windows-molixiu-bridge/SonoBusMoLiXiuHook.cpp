// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include <windows.h>

#include "SonoBusMoLiXiuFrame.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>

#if ! defined(_M_IX86)
#error "SonoBusMoLiXiuHook must be built as a 32-bit Windows DLL."
#endif

namespace
{
constexpr DWORD kMaxDeviceChars = 1023;
constexpr SIZE_T kSetCurrentDeviceHookLength = 5;
constexpr SIZE_T kStartCaptureHookLength = 6;
constexpr SIZE_T kStartCaptureWindowHookLength = 5;
constexpr SIZE_T kCurrentSolutionHookLength = 5;
constexpr SIZE_T kIsCaptureingHookLength = 6;
constexpr SIZE_T kOnVideoSourceHookLength = 5;
constexpr SIZE_T kSetDataCallbackHookLength = 5;
// ponytail: fixed MoLiXiu 2.0.2111.2402 layout; fail closed for any other binary.
constexpr DWORD kMoLiXiuImageTimestamp = 0x619e0b61;
constexpr DWORD kMoLiXiuImageSize = 0x0039b000;
constexpr SIZE_T kMoLiXiuSharedDataRva = 0x0033db24;
constexpr SIZE_T kSharedDataPointerOffset = 0x0c;

CRITICAL_SECTION pendingLock;
HANDLE pendingEvent = nullptr;
wchar_t pendingDevice[kMaxDeviceChars + 1] {};
DWORD pendingLength = 0;
bool pendingDirty = false;
volatile LONG bridgeReady = 0;
void* setCurrentDeviceTrampoline = nullptr;
void* startCaptureTrampoline = nullptr;
void* startCaptureWindowTrampoline = nullptr;
void* currentSolutionTrampoline = nullptr;
void* isCaptureingTrampoline = nullptr;
void* setDataCallbackTrampoline = nullptr;
void* callbackOriginals[2] {};
void* patchedCallbackVtable = nullptr;
HANDLE frameMapping = nullptr;
sonobus::molixiu::FrameHeader* frameHeader = nullptr;
volatile LONG frameNumber = 0;
volatile LONG latestFrameNumber = 0;
void* latestVideoData = nullptr;

void patchCallbackVtable(const void* vtable) noexcept;
void patchCallbackFromWeak(const void* weakPointer) noexcept;

bool appendText(wchar_t* target, SIZE_T capacity, SIZE_T& length, const wchar_t* value)
{
    if (value == nullptr) return false;
    while (*value != L'\0')
    {
        if (length + 1 >= capacity) return false;
        target[length++] = *value++;
    }
    target[length] = L'\0';
    return true;
}

bool statePath(wchar_t* path, SIZE_T capacity)
{
    wchar_t appData[MAX_PATH] {};
    const auto length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (length == 0 || length >= std::size(appData)) return false;

    SIZE_T used = 0;
    if (! appendText(path, capacity, used, appData)
        || ! appendText(path, capacity, used, L"\\SonoBus"))
        return false;
    CreateDirectoryW(path, nullptr);
    if (! appendText(path, capacity, used, L"\\molixiu-camera.txt")) return false;
    return true;
}

void writeState(const wchar_t* device)
{
    if (device == nullptr || *device == L'\0') return;
    char utf8[4096] {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, device, -1,
                                          utf8, static_cast<int>(std::size(utf8)), nullptr, nullptr);
    if (bytes <= 1) return;

    wchar_t finalPath[4096] {};
    if (! statePath(finalPath, std::size(finalPath))) return;
    wchar_t temporaryPath[4096] {};
    SIZE_T pathLength = 0;
    if (! appendText(temporaryPath, std::size(temporaryPath), pathLength, finalPath)
        || ! appendText(temporaryPath, std::size(temporaryPath), pathLength, L".tmp"))
        return;

    HANDLE file = CreateFileW(temporaryPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char header[64] {};
    const auto headerLength = wsprintfA(header, "pid=%lu\ndevice=", GetCurrentProcessId());
    DWORD written = 0;
    const bool headerWritten = WriteFile(file, header, static_cast<DWORD>(headerLength), &written, nullptr)
                            && written == static_cast<DWORD>(headerLength);
    const auto deviceBytes = static_cast<DWORD>(bytes - 1);
    const bool deviceWritten = headerWritten && WriteFile(file, utf8, deviceBytes, &written, nullptr)
                            && written == deviceBytes;
    const char newline = '\n';
    const bool newlineWritten = deviceWritten && WriteFile(file, &newline, 1, &written, nullptr) && written == 1;
    FlushFileBuffers(file);
    CloseHandle(file);
    if (newlineWritten)
        MoveFileExW(temporaryPath, finalPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    else
        DeleteFileW(temporaryPath);
}

// MoLiXiu was built with the VS2010 x86 std::wstring ABI. Reading its object as
// bytes avoids constructing it with the bridge's newer C++ runtime.
bool copyLegacyWString(const void* object, wchar_t* output, DWORD& length) noexcept
{
    if (object == nullptr) return false;
    __try
    {
        const auto bytes = static_cast<const unsigned char*>(object);
        const auto size = *reinterpret_cast<const DWORD*>(bytes + 16);
        const auto capacity = *reinterpret_cast<const DWORD*>(bytes + 20);
        if (size == 0 || size > kMaxDeviceChars) return false;
        if (capacity < size) return false;
        const auto* value = capacity < 8
                          ? reinterpret_cast<const wchar_t*>(bytes)
                          : *reinterpret_cast<const wchar_t* const*>(bytes);
        if (value == nullptr) return false;
        for (DWORD index = 0; index < size; ++index) output[index] = value[index];
        output[size] = L'\0';
        length = size;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool isVirtualDevice(const wchar_t* value) noexcept
{
    if (value == nullptr) return false;
    constexpr const wchar_t* virtualNames[] {
        L"yyanchorvcam", L"yyanchormulvcam", L"obs virtual camera", L"webcastmate virtualcamera"
    };
    for (const auto* name : virtualNames)
    {
        const auto length = std::wcslen(name);
        for (auto cursor = value; *cursor != L'\0'; ++cursor)
            if (_wcsnicmp(cursor, name, length) == 0) return true;
    }
    return false;
}

extern "C" bool __cdecl queueLegacyDevice(const void* object) noexcept
{
    if (InterlockedCompareExchange(&bridgeReady, 0, 0) == 0) return false;
    wchar_t value[kMaxDeviceChars + 1] {};
    DWORD length = 0;
    if (! copyLegacyWString(object, value, length)) return false;
    // Keep the last physical source when the host switches to a virtual/video source.
    if (isVirtualDevice(value)) return true;
    EnterCriticalSection(&pendingLock);
    if (length == pendingLength && std::memcmp(pendingDevice, value, (length + 1) * sizeof(wchar_t)) == 0)
    {
        LeaveCriticalSection(&pendingLock);
        return true;
    }
    std::memcpy(pendingDevice, value, (length + 1) * sizeof(wchar_t));
    pendingLength = length;
    pendingDirty = true;
    LeaveCriticalSection(&pendingLock);
    SetEvent(pendingEvent);
    return true;
}

const void* currentCameraFromSharedData(const unsigned char* sharedData) noexcept
{
    if (sharedData == nullptr) return nullptr;
    const auto state = *reinterpret_cast<const unsigned char* const*>(sharedData + kSharedDataPointerOffset);
    return state != nullptr ? *reinterpret_cast<const void* const*>(state) : nullptr;
}

extern "C" bool __cdecl queueCurrentDevice(const void* realCamera) noexcept
{
    if (realCamera == nullptr) return false;
    __try
    {
        const auto internal = *reinterpret_cast<const unsigned char* const*>(realCamera);
        // ponytail: fixed MoLiXiu 2021 private ABI; update this offset with the app version.
        if (internal == nullptr) return false;
        patchCallbackFromWeak(internal);
        return queueLegacyDevice(internal + 0xa0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool readable(const void* address, SIZE_T bytes) noexcept
{
    if (address == nullptr || bytes == 0) return false;
    auto* cursor = static_cast<const unsigned char*>(address);
    while (bytes != 0)
    {
        MEMORY_BASIC_INFORMATION info {};
        if (VirtualQuery(cursor, &info, sizeof(info)) != sizeof(info)
            || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;
        const auto regionEnd = static_cast<const unsigned char*>(info.BaseAddress) + info.RegionSize;
        if (cursor >= regionEnd) return false;
        const auto available = static_cast<SIZE_T>(regionEnd - cursor);
        if (available >= bytes) return true;
        bytes -= available;
        cursor = regionEnd;
    }
    return true;
}

bool readWord(const unsigned char* object, SIZE_T offset, DWORD& value) noexcept
{
    __try
    {
        value = *reinterpret_cast<const DWORD*>(object + offset);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct RawFrame
{
    const unsigned char* data = nullptr;
    DWORD bytes = 0;
    DWORD stride = 0;
    DWORD pixelFormat = sonobus::molixiu::kPixelBgr24;
};

bool classifyRawFrame(const unsigned char* data, DWORD bytes, DWORD width, DWORD height, RawFrame& output) noexcept
{
    if (data == nullptr || bytes == 0 || width < 2 || height < 2
        || width > 8192 || height > 8192 || bytes > sonobus::molixiu::kMaxFrameBytes)
        return false;

    const auto pixels = static_cast<std::uint64_t>(width) * height;
    const DWORD packedFormats[] { sonobus::molixiu::kPixelBgr24, sonobus::molixiu::kPixelBgra32,
                                  sonobus::molixiu::kPixelYuy2, sonobus::molixiu::kPixelNv12 };
    const std::uint64_t packedSizes[] { pixels * 3, pixels * 4, pixels * 2, pixels * 3 / 2 };
    for (size_t index = 0; index < std::size(packedFormats); ++index)
    {
        if (packedSizes[index] != bytes) continue;
        if (! readable(data, bytes)) return false;
        output = { data, bytes, index == 3 ? width : static_cast<DWORD>(packedSizes[index] / height),
                   packedFormats[index] };
        return true;
    }

    if (bytes % height != 0) return false;
    const auto stride = bytes / height;
    for (size_t index = 0; index < 3; ++index)
    {
        const auto rowBytes = packedSizes[index] / height;
        if (stride < rowBytes || stride > rowBytes + 4096) continue;
        if (! readable(data, bytes)) return false;
        output = { data, bytes, stride, packedFormats[index] };
        return true;
    }
    return false;
}

bool tryMoLiXiuImageBlock(const unsigned char* object, DWORD& width, DWORD& height,
                          RawFrame& output) noexcept
{
    DWORD imageBlock = 0;
    if (! readWord(object, 0x08, imageBlock) || imageBlock == 0) return false;

    const auto* image = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(imageBlock));
    DWORD bytes = 0;
    DWORD data = 0;
    // MoLiXiu embeds Qt 4 QImage. Its QImageData::bits() pointer is at +0x18;
    // +0x14 is not pixel data.
    if (! readWord(image, 0x04, width) || ! readWord(image, 0x08, height)
        || ! readWord(image, 0x10, bytes) || ! readWord(image, 0x18, data))
        return false;
    return classifyRawFrame(reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(data)),
                            bytes, width, height, output);
}

bool copyMoLiXiuFrame(const void* videoData) noexcept
{
    if (frameHeader == nullptr || videoData == nullptr) return false;
    __try
    {
        const auto* object = static_cast<const unsigned char*>(videoData);
        DWORD width = 0;
        DWORD height = 0;
        RawFrame source;
        if (! tryMoLiXiuImageBlock(object, width, height, source)) return false;

        const auto sequence = static_cast<DWORD>(InterlockedIncrement(&frameNumber) * 2 - 1);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&frameHeader->sequence), static_cast<LONG>(sequence));
        auto* destination = reinterpret_cast<unsigned char*>(frameHeader) + sizeof(*frameHeader);
        const auto rowBytes = source.pixelFormat == sonobus::molixiu::kPixelBgr24 ? width * 3
                             : source.pixelFormat == sonobus::molixiu::kPixelBgra32 ? width * 4
                             : source.pixelFormat == sonobus::molixiu::kPixelYuy2 ? width * 2
                             : source.bytes;
        if (source.pixelFormat == sonobus::molixiu::kPixelNv12 || source.stride == rowBytes)
            std::memcpy(destination, source.data, source.bytes);
        else
            for (DWORD row = 0; row < height; ++row)
                std::memcpy(destination + static_cast<SIZE_T>(row) * rowBytes,
                            source.data + static_cast<SIZE_T>(row) * source.stride, rowBytes);

        frameHeader->magic = sonobus::molixiu::kFrameMagic;
        frameHeader->version = sonobus::molixiu::kFrameVersion;
        frameHeader->width = width;
        frameHeader->height = height;
        frameHeader->stride = source.pixelFormat == sonobus::molixiu::kPixelNv12 ? width : rowBytes;
        frameHeader->pixelFormat = source.pixelFormat;
        frameHeader->bytes = source.pixelFormat == sonobus::molixiu::kPixelNv12
                           ? source.bytes : rowBytes * height;
        frameHeader->frameNumber = static_cast<DWORD>(InterlockedCompareExchange(&frameNumber, 0, 0));
        frameHeader->reserved = 30000; // nominal source rate in milli-fps
        MemoryBarrier();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&frameHeader->sequence), sequence + 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

const void* resolveCallbackFrame(const void* stack) noexcept
{
    if (stack == nullptr) return nullptr;
    __try
    {
        const auto* words = static_cast<const DWORD*>(stack);
        // SourceDataCallBack is an x86 __thiscall method: ECX is `this` and
        // the first real argument starts at the first stack word after ret.
        const auto firstArgument = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(words[1]));
        if (firstArgument == nullptr) return nullptr;
        DWORD width = 0;
        DWORD height = 0;
        RawFrame source;
        if (tryMoLiXiuImageBlock(static_cast<const unsigned char*>(firstArgument), width, height, source))
            return firstArgument;

        // A boost::shared_ptr can be passed either by value (raw VideoData is the
        // first stack word) or by reference (the first word points to that raw
        // VideoData pointer). Accept both ABI forms without touching ownership.
        DWORD rawVideoData = 0;
        if (readWord(static_cast<const unsigned char*>(firstArgument), 0, rawVideoData))
            return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(rawVideoData));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return nullptr;
}

void rememberCallbackFrame(const void* stack) noexcept
{
    const auto videoData = resolveCallbackFrame(stack);
    if (videoData != nullptr)
    {
        InterlockedExchangePointer(&latestVideoData, const_cast<void*>(videoData));
        InterlockedIncrement(&latestFrameNumber);
    }
}

DWORD WINAPI frameCopyThread(void*)
{
    LONG copiedFrameNumber = 0;
    for (;;)
    {
        const auto currentFrameNumber = InterlockedCompareExchange(&latestFrameNumber, 0, 0);
        if (currentFrameNumber != copiedFrameNumber)
        {
            Sleep(2);
            const auto videoData = InterlockedCompareExchangePointer(&latestVideoData, nullptr, nullptr);
            if (videoData != nullptr && copyMoLiXiuFrame(videoData)) copiedFrameNumber = currentFrameNumber;
        }
        Sleep(3);
    }
}

bool createFrameMapping() noexcept
{
    if (frameHeader != nullptr) return true;
    frameMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sonobus::molixiu::kFrameMappingBytes,
                                      sonobus::molixiu::kFrameMappingName);
    if (frameMapping == nullptr) return false;
    frameHeader = static_cast<sonobus::molixiu::FrameHeader*>(MapViewOfFile(
        frameMapping, FILE_MAP_ALL_ACCESS, 0, 0, sonobus::molixiu::kFrameMappingBytes));
    if (frameHeader == nullptr)
    {
        CloseHandle(frameMapping);
        frameMapping = nullptr;
        return false;
    }
    std::memset(frameHeader, 0, sizeof(*frameHeader));
    return true;
}

extern "C" __declspec(naked) void hookCallback0()
{
    __asm
    {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        call rememberCallbackFrame
        add esp, 4
        popad
        popfd
        jmp dword ptr [callbackOriginals + 0]
    }
}
extern "C" __declspec(naked) void hookCallback1()
{
    __asm
    {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        call rememberCallbackFrame
        add esp, 4
        popad
        popfd
        jmp dword ptr [callbackOriginals + 4]
    }
}

void patchCallbackVtable(const void* vtable) noexcept
{
    if (vtable == nullptr || InterlockedCompareExchangePointer(&patchedCallbackVtable,
                                                                 const_cast<void*>(vtable), nullptr) != nullptr)
        return;
    // The 2021 callback ABI has two candidate video methods across the x86
    // builds seen in the wild. Hook both; each wrapper preserves its own
    // original entry and only copies a frame when the argument matches.
    const auto* entries = reinterpret_cast<const void* const*>(vtable);
    const void* wrappers[] { reinterpret_cast<const void*>(&hookCallback0),
                             reinterpret_cast<const void*>(&hookCallback1) };
    int patched = 0;
    for (int index = 0; index < 2; ++index)
    {
        const auto entry = entries[index];
        if (entry == nullptr) continue;
        DWORD oldProtection = 0;
        auto* slot = reinterpret_cast<void**>(const_cast<void*>(vtable)) + index;
        if (! VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) continue;
        callbackOriginals[index] = const_cast<void*>(entry);
        *slot = const_cast<void*>(wrappers[index]);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
        ++patched;
    }
    if (patched == 0)
        InterlockedExchangePointer(&patchedCallbackVtable, nullptr);
    else
        FlushInstructionCache(GetCurrentProcess(), const_cast<void*>(vtable), sizeof(void*) * 2);
}

void patchCallbackFromWeak(const void* weakPointer) noexcept
{
    if (weakPointer == nullptr) return;
    __try
    {
        const auto* words = static_cast<const void* const*>(weakPointer);
        const auto callback = words[0];
        if (callback == nullptr) return;
        const auto vtable = *reinterpret_cast<const void* const*>(callback);
        patchCallbackVtable(vtable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool queueExistingSelection() noexcept
{
    const auto module = GetModuleHandleW(L"molixiudll.dll");
    if (module == nullptr) return false;
    __try
    {
        const auto base = reinterpret_cast<const unsigned char*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.TimeDateStamp != kMoLiXiuImageTimestamp
            || nt->OptionalHeader.SizeOfImage != kMoLiXiuImageSize)
            return false;
        return queueCurrentDevice(currentCameraFromSharedData(base + kMoLiXiuSharedDataRva));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

extern "C" __declspec(naked) void hookSetCurrentDevice()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 40]
        push edx
        call queueLegacyDevice
        add esp, 4
        popad
        popfd
        jmp dword ptr [setCurrentDeviceTrampoline]
    }
}

extern "C" __declspec(naked) void hookStartCapture()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 40]
        push edx
        call queueLegacyDevice
        add esp, 4
        popad
        popfd
        jmp dword ptr [startCaptureTrampoline]
    }
}

extern "C" __declspec(naked) void hookStartCaptureWindow()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 48]
        push edx
        call queueLegacyDevice
        add esp, 4
        popad
        popfd
        jmp dword ptr [startCaptureWindowTrampoline]
    }
}

extern "C" __declspec(naked) void hookCurrentSolution()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 24]
        push edx
        call queueCurrentDevice
        add esp, 4
        popad
        popfd
        jmp dword ptr [currentSolutionTrampoline]
    }
}

extern "C" __declspec(naked) void hookIsCaptureing()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 24]
        push edx
        call queueCurrentDevice
        add esp, 4
        popad
        popfd
        jmp dword ptr [isCaptureingTrampoline]
    }
}

extern "C" __declspec(naked) void hookSetDataCallback()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 40]
        push edx
        call patchCallbackFromWeak
        add esp, 4
        popad
        popfd
        jmp dword ptr [setDataCallbackTrampoline]
    }
}

bool installHook(void* target, void* replacement, SIZE_T length, const unsigned char* expected, void** trampoline)
{
    if (target == nullptr || replacement == nullptr || trampoline == nullptr || length < 5) return false;
    if (std::memcmp(target, expected, length) != 0) return false;

    auto* copy = static_cast<unsigned char*>(VirtualAlloc(nullptr, length + 5, MEM_RESERVE | MEM_COMMIT,
                                                          PAGE_EXECUTE_READWRITE));
    if (copy == nullptr) return false;
    std::memcpy(copy, target, length);
    copy[length] = 0xE9;
    *reinterpret_cast<std::int32_t*>(copy + length + 1) =
        static_cast<std::int32_t>(static_cast<unsigned char*>(target) + length - (copy + length + 5));

    DWORD oldProtection = 0;
    if (! VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        VirtualFree(copy, 0, MEM_RELEASE);
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(target);
    bytes[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(bytes + 1) =
        static_cast<std::int32_t>(static_cast<unsigned char*>(replacement) - (bytes + 5));
    for (SIZE_T index = 5; index < length; ++index) bytes[index] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, length);
    DWORD ignored = 0;
    VirtualProtect(target, length, oldProtection, &ignored);
    *trampoline = copy;
    return true;
}

void copyPendingAndWrite()
{
    wchar_t value[kMaxDeviceChars + 1] {};
    EnterCriticalSection(&pendingLock);
    if (! pendingDirty)
    {
        LeaveCriticalSection(&pendingLock);
        return;
    }
    const auto length = pendingLength;
    std::memcpy(value, pendingDevice, (length + 1) * sizeof(wchar_t));
    pendingDirty = false;
    LeaveCriticalSection(&pendingLock);
    writeState(value);
}

DWORD WINAPI bridgeThread(void*)
{
    InitializeCriticalSection(&pendingLock);
    pendingEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (pendingEvent == nullptr) return 0;

    HMODULE cameraCore = nullptr;
    for (int attempt = 0; attempt < 300 && cameraCore == nullptr; ++attempt)
    {
        cameraCore = GetModuleHandleW(L"CameraCore.dll");
        if (cameraCore == nullptr) Sleep(100);
    }
    if (cameraCore == nullptr) return 0;
    if (createFrameMapping()) CreateThread(nullptr, 0, frameCopyThread, nullptr, 0, nullptr);

    const auto setCurrentDevice = GetProcAddress(cameraCore,
        "?setCurrentDevice@RealCamera@@QAEXABV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@@Z");
    const auto startCapture = GetProcAddress(cameraCore,
        "?startCapture@RealCamera@@QAEXAAV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@@Z");
    const auto startCaptureWindow = GetProcAddress(cameraCore,
        "?startCapture@RealCamera@@QAEXPAUHWND__@@ABUtagRECT@@AAV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@@Z");
    const auto currentSolution = GetProcAddress(cameraCore,
        "?getCurrentSolution@RealCamera@@QAEHXZ");
    const auto isCaptureing = GetProcAddress(cameraCore,
        "?isCaptureing@RealCamera@@QAE_NXZ");
    const auto setDataCallback = GetProcAddress(cameraCore,
        "?setDataCallback@RealCamera@@QAEXAAV?$weak_ptr@VSourceDataCallBack@@@boost@@@Z");
    const unsigned char setExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x09 };
    const unsigned char startExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
    const unsigned char windowExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x09 };
    if (! installHook(reinterpret_cast<void*>(setCurrentDevice), reinterpret_cast<void*>(&hookSetCurrentDevice),
                      kSetCurrentDeviceHookLength, setExpected, &setCurrentDeviceTrampoline)
        || ! installHook(reinterpret_cast<void*>(startCapture), reinterpret_cast<void*>(&hookStartCapture),
                         kStartCaptureHookLength, startExpected, &startCaptureTrampoline)
        || ! installHook(reinterpret_cast<void*>(startCaptureWindow), reinterpret_cast<void*>(&hookStartCaptureWindow),
                         kStartCaptureWindowHookLength, windowExpected, &startCaptureWindowTrampoline))
        return 0;

    const unsigned char currentSolutionExpected[] = { 0x8B, 0x09, 0x8B, 0x41, 0x08 };
    const unsigned char isCaptureingExpected[] = { 0x8B, 0x01, 0x83, 0x78, 0x18, 0x00 };
    installHook(reinterpret_cast<void*>(currentSolution), reinterpret_cast<void*>(&hookCurrentSolution),
                kCurrentSolutionHookLength, currentSolutionExpected, &currentSolutionTrampoline);
    installHook(reinterpret_cast<void*>(isCaptureing), reinterpret_cast<void*>(&hookIsCaptureing),
                kIsCaptureingHookLength, isCaptureingExpected, &isCaptureingTrampoline);
    const unsigned char setDataCallbackExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45 };
    installHook(reinterpret_cast<void*>(setDataCallback), reinterpret_cast<void*>(&hookSetDataCallback),
                kSetDataCallbackHookLength, setDataCallbackExpected, &setDataCallbackTrampoline);

    InterlockedExchange(&bridgeReady, 1);
    for (int attempt = 0; attempt < 50 && ! queueExistingSelection(); ++attempt) Sleep(100);
    for (;;)
    {
        if (WaitForSingleObject(pendingEvent, INFINITE) == WAIT_OBJECT_0) copyPendingAndWrite();
    }
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, bridgeThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl SonoBusMoLiXiuLayoutSelfTest()
{
    if (sizeof(sonobus::molixiu::FrameHeader) != 40
        || sonobus::molixiu::kFrameMappingBytes <= sonobus::molixiu::kMaxFrameBytes)
        return 2;
    unsigned char sharedData[sizeof(void*) + kSharedDataPointerOffset] {};
    unsigned char state[sizeof(void*)] {};
    int camera = 0;
    *reinterpret_cast<const unsigned char**>(sharedData + kSharedDataPointerOffset) = state;
    *reinterpret_cast<const void**>(state) = &camera;
    return currentCameraFromSharedData(sharedData) == &camera ? 0 : 1;
}
