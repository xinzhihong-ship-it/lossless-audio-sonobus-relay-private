// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include <windows.h>

#include <cstdint>
#include <cstring>
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
void* onVideoSourceTrampoline = nullptr;
void* setDataCallbackTrampoline = nullptr;
volatile LONG videoProbeCount = 0;
volatile LONG callbackProbeCount = 0;

void appendCallbackProbe(const char* source, const void* callback, const void* control) noexcept;

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

extern "C" bool __cdecl queueLegacyDevice(const void* object) noexcept
{
    if (InterlockedCompareExchange(&bridgeReady, 0, 0) == 0) return false;
    wchar_t value[kMaxDeviceChars + 1] {};
    DWORD length = 0;
    if (! copyLegacyWString(object, value, length)) return false;
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
        appendCallbackProbe("existing", *reinterpret_cast<const void* const*>(internal),
                            *reinterpret_cast<const void* const*>(internal + 0x4));
        return queueLegacyDevice(internal + 0xa0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void appendVideoProbe(const void* videoData) noexcept
{
    if (videoData == nullptr || InterlockedIncrement(&videoProbeCount) > 30) return;
    __try
    {
        wchar_t appData[MAX_PATH] {};
        const auto appDataLength = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
        if (appDataLength == 0 || appDataLength >= std::size(appData)) return;
        wchar_t directory[MAX_PATH * 2] {};
        if (lstrcpyW(directory, appData) == nullptr || lstrcatW(directory, L"\\SonoBus") == nullptr) return;
        CreateDirectoryW(directory, nullptr);
        wchar_t path[MAX_PATH * 2] {};
        if (lstrcpyW(path, directory) == nullptr || lstrcatW(path, L"\\molixiu-video-probe.txt") == nullptr) return;
        const auto file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;

        char line[2048] {};
        int used = wsprintfA(line, "video=%p", videoData);
        const auto bytes = static_cast<const unsigned char*>(videoData);
        for (int index = 0; index < 128 && used + 4 < static_cast<int>(std::size(line)); ++index)
            used += wsprintfA(line + used, " %02X", bytes[index]);
        line[used++] = '\r';
        line[used++] = '\n';
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(used), &written, nullptr);
        CloseHandle(file);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void appendCallbackProbe(const char* source, const void* callback, const void* control) noexcept
{
    if (callback == nullptr || InterlockedIncrement(&callbackProbeCount) > 12) return;
    __try
    {
        wchar_t appData[MAX_PATH] {};
        const auto appDataLength = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
        if (appDataLength == 0 || appDataLength >= std::size(appData)) return;
        wchar_t directory[MAX_PATH * 2] {};
        if (lstrcpyW(directory, appData) == nullptr || lstrcatW(directory, L"\\SonoBus") == nullptr) return;
        CreateDirectoryW(directory, nullptr);
        wchar_t path[MAX_PATH * 2] {};
        if (lstrcpyW(path, directory) == nullptr || lstrcatW(path, L"\\molixiu-callback-probe.txt") == nullptr) return;
        const auto file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;

        char line[4096] {};
        int used = wsprintfA(line, "source=%s callback=%p control=%p", source != nullptr ? source : "unknown",
                             callback, control);
        const auto object = static_cast<const unsigned char*>(callback);
        for (int index = 0; index < 64 && used + 4 < static_cast<int>(std::size(line)); ++index)
            used += wsprintfA(line + used, " %02X", object[index]);
        const auto vtable = *reinterpret_cast<const void* const*>(callback);
        used += wsprintfA(line + used, " vtable=%p", vtable);
        const auto* entries = static_cast<const void* const*>(vtable);
        for (int index = 0; index < 16 && used + 24 < static_cast<int>(std::size(line)); ++index)
            used += wsprintfA(line + used, " v%d=%p", index, entries[index]);
        line[used++] = '\r';
        line[used++] = '\n';
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(used), &written, nullptr);
        CloseHandle(file);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void probeWeakCallback(const void* weakPointer) noexcept
{
    if (weakPointer == nullptr) return;
    __try
    {
        const auto* words = static_cast<const void* const*>(weakPointer);
        appendCallbackProbe("setDataCallback", words[0], words[1]);
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

extern "C" __declspec(naked) void hookOnVideoSource()
{
    __asm
    {
        pushfd
        pushad
        mov edx, [esp + 40]
        mov edx, [edx]
        push edx
        call appendVideoProbe
        add esp, 4
        popad
        popfd
        jmp dword ptr [onVideoSourceTrampoline]
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
        call probeWeakCallback
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
    const auto onVideoSource = GetProcAddress(cameraCore,
        "?onVideoSource@VideoDataProcess@@QAEXAAV?$shared_ptr@UVideoData@@@boost@@@Z");
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
    const unsigned char onVideoSourceExpected[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    installHook(reinterpret_cast<void*>(onVideoSource), reinterpret_cast<void*>(&hookOnVideoSource),
                kOnVideoSourceHookLength, onVideoSourceExpected, &onVideoSourceTrampoline);
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
    unsigned char sharedData[sizeof(void*) + kSharedDataPointerOffset] {};
    unsigned char state[sizeof(void*)] {};
    int camera = 0;
    *reinterpret_cast<const unsigned char**>(sharedData + kSharedDataPointerOffset) = state;
    *reinterpret_cast<const void**>(state) = &camera;
    return currentCameraFromSharedData(sharedData) == &camera ? 0 : 1;
}
