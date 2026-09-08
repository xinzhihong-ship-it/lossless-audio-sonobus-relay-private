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
struct CallbackPatch
{
    CallbackPatch* next = nullptr;
    void* vtable = nullptr;
    void* originals[2] {};
};
// Records are immutable after publication and intentionally retained until the
// host process exits. MoLiXiu may destroy a callback object immediately after
// replacing it, so reclaiming a vtable record would make an in-flight wrapper
// jump through freed metadata.
CallbackPatch* volatile callbackPatches = nullptr;
volatile LONG callbackPatchLock = 0;
HMODULE cameraCoreReference = nullptr;
HANDLE frameMapping = nullptr;
sonobus::molixiu::FrameHeader* frameHeader = nullptr;
volatile LONG frameNumber = 0;

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

void copyCallbackFrame(const void* stack) noexcept
{
    const auto videoData = resolveCallbackFrame(stack);
    if (videoData != nullptr) copyMoLiXiuFrame(videoData);
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

bool readPointer(const void* address, void*& value) noexcept
{
    value = nullptr;
    if (address == nullptr) return false;
    DWORD raw = 0;
    if (! readWord(static_cast<const unsigned char*>(address), 0, raw)) return false;
    value = reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw));
    return true;
}

CallbackPatch* loadCallbackPatches() noexcept
{
    return static_cast<CallbackPatch*>(InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&callbackPatches), nullptr, nullptr));
}

CallbackPatch* latestCallbackPatch(const void* vtable) noexcept
{
    for (auto* patch = loadCallbackPatches(); patch != nullptr; patch = patch->next)
        if (patch->vtable == vtable) return patch;
    return nullptr;
}

extern "C" void* __cdecl callbackOriginalForObject(const void* object, int index) noexcept
{
    if (index < 0 || index >= 2) return nullptr;
    void* vtable = nullptr;
    if (readPointer(object, vtable))
    {
        if (const auto* patch = latestCallbackPatch(vtable); patch != nullptr
            && patch->originals[index] != nullptr)
            return patch->originals[index];
    }
    // A callback can only reach a wrapper after its vtable has been published,
    // but keep the original entry as a fail-safe for a racing object teardown.
    return callbackOriginals[index];
}

extern "C" __declspec(naked) void hookCallback0()
{
    __asm
    {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        call copyCallbackFrame
        add esp, 4
        mov eax, [esp + 24]
        push 0
        push eax
        call callbackOriginalForObject
        add esp, 8
        // [esp + 12] is PUSHAD's saved-ESP slot; POPAD skips it. Keep the
        // selected target there so the caller's original EAX is preserved.
        mov [esp + 12], eax
        popad
        popfd
        jmp dword ptr [esp - 24]
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
        call copyCallbackFrame
        add esp, 4
        mov eax, [esp + 24]
        push 1
        push eax
        call callbackOriginalForObject
        add esp, 8
        // [esp + 12] is PUSHAD's saved-ESP slot; POPAD skips it. Keep the
        // selected target there so the caller's original EAX is preserved.
        mov [esp + 12], eax
        popad
        popfd
        jmp dword ptr [esp - 24]
    }
}

bool writeCallbackVtableEntry(void* vtable, int index, void* value) noexcept
{
    if (vtable == nullptr || index < 0 || index >= 2) return false;
    auto* slot = reinterpret_cast<void**>(vtable) + index;
    DWORD oldProtection = 0;
    if (! VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) return false;

    bool written = false;
    __try
    {
        *slot = value;
        written = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    return written;
}

void patchCallbackVtable(const void* vtable) noexcept
{
    if (vtable == nullptr || InterlockedCompareExchange(&callbackPatchLock, 1, 0) != 0) return;

    auto unlock = []() noexcept { InterlockedExchange(&callbackPatchLock, 0); };
    auto* candidate = const_cast<void*>(vtable);
    auto* previous = latestCallbackPatch(candidate);
    const void* entries[2] {};
    const void* wrappers[] { reinterpret_cast<const void*>(&hookCallback0),
                             reinterpret_cast<const void*>(&hookCallback1) };
    void* originals[2] {};
    bool hasEntry = false;
    bool needsPatch = false;

    // Read the entries through SEH-protected loads. If the callback is already
    // being destroyed, fail closed without leaving the patch lock held.
    for (int index = 0; index < 2; ++index)
    {
        void* entry = nullptr;
        if (! readPointer(reinterpret_cast<const unsigned char*>(candidate)
                          + static_cast<SIZE_T>(index) * sizeof(void*), entry))
        {
            unlock();
            return;
        }
        entries[index] = entry;
        if (entry == nullptr) continue;
        hasEntry = true;
        if (entry == wrappers[index])
        {
            if (previous == nullptr || previous->originals[index] == nullptr)
            {
                unlock();
                return;
            }
            originals[index] = previous->originals[index];
        }
        else
        {
            needsPatch = true;
            originals[index] = entry;
        }
    }
    if (! hasEntry || ! needsPatch) {
        unlock();
        return;
    }

    // Publish immutable metadata before exposing either wrapper in the vtable.
    // A frame callback can begin on another thread immediately after the first
    // slot is changed, so it must never observe a wrapper without its original.
    auto* patch = static_cast<CallbackPatch*>(VirtualAlloc(nullptr, sizeof(CallbackPatch),
                                                            MEM_RESERVE | MEM_COMMIT,
                                                            PAGE_READWRITE));
    if (patch == nullptr)
    {
        unlock();
        return;
    }
    patch->vtable = candidate;
    patch->originals[0] = originals[0];
    patch->originals[1] = originals[1];
    patch->next = loadCallbackPatches();
    callbackOriginals[0] = originals[0] != nullptr ? originals[0] : callbackOriginals[0];
    callbackOriginals[1] = originals[1] != nullptr ? originals[1] : callbackOriginals[1];
    MemoryBarrier();
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&callbackPatches), patch);

    bool patched[2] {};
    for (int index = 0; index < 2; ++index)
    {
        if (entries[index] == nullptr || entries[index] == wrappers[index]) continue;
        if (! writeCallbackVtableEntry(candidate, index, const_cast<void*>(wrappers[index])))
        {
            // Keep the immutable record: if rollback races with an in-flight
            // callback, it still contains the correct original target. A later
            // pass can retry any slot that remains unpatched.
            for (int rollback = 0; rollback < 2; ++rollback)
                if (patched[rollback])
                    writeCallbackVtableEntry(candidate, rollback, const_cast<void*>(entries[rollback]));
            FlushInstructionCache(GetCurrentProcess(), candidate, sizeof(void*) * 2);
            unlock();
            return;
        }
        patched[index] = true;
    }
    FlushInstructionCache(GetCurrentProcess(), candidate, sizeof(void*) * 2);
    unlock();
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
    FlushInstructionCache(GetCurrentProcess(), copy, length + 5);

    // The hook can be entered as soon as the target's first byte becomes a
    // jump. Publish its trampoline first so that an early caller never sees a
    // valid replacement with a null trampoline.
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(trampoline), copy);
    MemoryBarrier();

    DWORD oldProtection = 0;
    if (! VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(trampoline), nullptr);
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
    return true;
}

bool hookAlreadyInstalled(void* target, void* replacement) noexcept
{
    if (target == nullptr || replacement == nullptr) return false;
    unsigned char bytes[5] {};
    std::memcpy(bytes, target, sizeof(bytes));
    if (bytes[0] != 0xE9) return false;
    const auto relative = *reinterpret_cast<const std::int32_t*>(bytes + 1);
    const auto destination = reinterpret_cast<std::uintptr_t>(target) + 5
                           + static_cast<std::intptr_t>(relative);
    return reinterpret_cast<void*>(destination) == replacement;
}

bool ensureHook(void* target, void* replacement, SIZE_T length, const unsigned char* expected,
                void** trampoline, bool& changed)
{
    if (hookAlreadyInstalled(target, replacement)) return *trampoline != nullptr;
    if (! installHook(target, replacement, length, expected, trampoline)) return false;
    changed = true;
    return true;
}

bool installCameraCoreHooks(HMODULE cameraCore, bool& changed)
{
    if (cameraCore == nullptr) return false;
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
    const unsigned char currentSolutionExpected[] = { 0x8B, 0x09, 0x8B, 0x41, 0x08 };
    const unsigned char isCaptureingExpected[] = { 0x8B, 0x01, 0x83, 0x78, 0x18, 0x00 };
    const unsigned char setDataCallbackExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45 };

    const bool setCurrentDeviceReady = ensureHook(
        reinterpret_cast<void*>(setCurrentDevice), reinterpret_cast<void*>(&hookSetCurrentDevice),
        kSetCurrentDeviceHookLength, setExpected, &setCurrentDeviceTrampoline, changed);
    const bool startCaptureReady = ensureHook(
        reinterpret_cast<void*>(startCapture), reinterpret_cast<void*>(&hookStartCapture),
        kStartCaptureHookLength, startExpected, &startCaptureTrampoline, changed);
    const bool startCaptureWindowReady = ensureHook(
        reinterpret_cast<void*>(startCaptureWindow), reinterpret_cast<void*>(&hookStartCaptureWindow),
        kStartCaptureWindowHookLength, windowExpected, &startCaptureWindowTrampoline, changed);
    const bool currentSolutionReady = ensureHook(
        reinterpret_cast<void*>(currentSolution), reinterpret_cast<void*>(&hookCurrentSolution),
        kCurrentSolutionHookLength, currentSolutionExpected, &currentSolutionTrampoline, changed);
    const bool isCaptureingReady = ensureHook(
        reinterpret_cast<void*>(isCaptureing), reinterpret_cast<void*>(&hookIsCaptureing),
        kIsCaptureingHookLength, isCaptureingExpected, &isCaptureingTrampoline, changed);
    const bool setDataCallbackReady = ensureHook(
        reinterpret_cast<void*>(setDataCallback), reinterpret_cast<void*>(&hookSetDataCallback),
        kSetDataCallbackHookLength, setDataCallbackExpected, &setDataCallbackTrampoline, changed);
    return setCurrentDeviceReady && startCaptureReady && startCaptureWindowReady
        && currentSolutionReady && isCaptureingReady && setDataCallbackReady;
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
    createFrameMapping();
    bool selectionPending = true;

    for (;;)
    {
        // Hold a module reference for the lifetime of the injected hook. A bare
        // GetModuleHandleW result can become dangling while close/reopen is
        // unloading CameraCore between GetProcAddress and the patch write.
        if (cameraCoreReference == nullptr)
        {
            HMODULE discovered = nullptr;
            if (GetModuleHandleExW(0, L"CameraCore.dll", &discovered))
                cameraCoreReference = discovered;
        }

        bool changed = false;
        const auto hooksReady = cameraCoreReference != nullptr
                             && installCameraCoreHooks(cameraCoreReference, changed);
        if (hooksReady)
        {
            InterlockedExchange(&bridgeReady, 1);
            if (changed) selectionPending = true;
            // The camera may already be open when the hook is installed. Keep
            // retrying until its private state has produced a real camera
            // object; later close/reopen transitions are handled by the hooks.
            if (selectionPending && queueExistingSelection())
                selectionPending = false;
        }
        if (WaitForSingleObject(pendingEvent, 250) == WAIT_OBJECT_0)
            copyPendingAndWrite();
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

void callbackSelfTestOriginal0() noexcept {}
void callbackSelfTestOriginal1() noexcept {}
void callbackSelfTestOriginal2() noexcept {}
void callbackSelfTestOriginal3() noexcept {}

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
    if (currentCameraFromSharedData(sharedData) != &camera) return 1;

    // Closing/reopening the camera can give MoLiXiu a fresh callback vtable.
    // Keep the two original targets independent, as the live wrappers do.
    static void* firstVtable[2] {
        reinterpret_cast<void*>(&callbackSelfTestOriginal0),
        reinterpret_cast<void*>(&callbackSelfTestOriginal1)
    };
    static void* secondVtable[2] {
        reinterpret_cast<void*>(&callbackSelfTestOriginal2),
        reinterpret_cast<void*>(&callbackSelfTestOriginal3)
    };
    void* firstObject[1] { firstVtable };
    void* secondObject[1] { secondVtable };
    patchCallbackVtable(firstVtable);
    patchCallbackVtable(secondVtable);
    if (firstVtable[0] != reinterpret_cast<void*>(&hookCallback0)
        || secondVtable[0] != reinterpret_cast<void*>(&hookCallback0)
        || callbackOriginalForObject(firstObject, 0) != reinterpret_cast<void*>(&callbackSelfTestOriginal0)
        || callbackOriginalForObject(secondObject, 0) != reinterpret_cast<void*>(&callbackSelfTestOriginal2))
        return 3;
    return 0;
}
