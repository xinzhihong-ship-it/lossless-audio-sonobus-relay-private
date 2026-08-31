// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwchar>
#include <cstring>
#include <string>

namespace
{
constexpr wchar_t hookName[] = L"SonoBusMoLiXiuHook.dll";
constexpr DWORD maxDeviceChars = 1023;
constexpr DWORD moLiXiuImageTimestamp = 0x619e0b61;
constexpr DWORD moLiXiuImageSize = 0x0039b000;
constexpr std::uintptr_t moLiXiuSharedDataRva = 0x0033db24;
constexpr std::uintptr_t sharedDataPointerOffset = 0x0c;
constexpr std::uintptr_t currentDeviceOffset = 0xa0;

static_assert(sizeof(void*) == 4, "SonoBusMoLiXiuBridge must be built as a 32-bit executable.");

DWORD findProcess()
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry { sizeof(entry) };
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"molixiu.exe") == 0)
            {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool findModule(DWORD pid, const wchar_t* name, MODULEENTRY32W& result)
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry { sizeof(entry) };
    bool found = false;
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, name) == 0)
            {
                result = entry;
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool alreadyLoaded(DWORD pid)
{
    MODULEENTRY32W module { sizeof(module) };
    return findModule(pid, hookName, module);
}

bool readExact(HANDLE process, std::uintptr_t address, void* output, SIZE_T bytes)
{
    SIZE_T read = 0;
    return address != 0
        && ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), output, bytes, &read)
        && read == bytes;
}

template <typename Value>
bool readValue(HANDLE process, std::uintptr_t address, Value& value)
{
    return readExact(process, address, &value, sizeof(value));
}

bool readLegacyWString(HANDLE process, std::uintptr_t objectAddress, std::wstring& output)
{
    unsigned char object[24] {};
    if (! readExact(process, objectAddress, object, sizeof(object))) return false;

    DWORD size = 0;
    DWORD capacity = 0;
    std::memcpy(&size, object + 16, sizeof(size));
    std::memcpy(&capacity, object + 20, sizeof(capacity));
    if (size == 0 || size > maxDeviceChars || capacity < size) return false;

    auto valueAddress = objectAddress;
    if (capacity >= 8) std::memcpy(&valueAddress, object, sizeof(valueAddress));
    std::wstring value(size, L'\0');
    if (! readExact(process, valueAddress, value.data(), size * sizeof(wchar_t))
        || value.find(L'\0') != std::wstring::npos)
        return false;
    output = value;
    return true;
}

bool readSelectedCamera(DWORD pid, std::wstring& output)
{
    MODULEENTRY32W module { sizeof(module) };
    if (! findModule(pid, L"molixiudll.dll", module) || module.modBaseSize != moLiXiuImageSize)
        return false;

    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) return false;

    const auto base = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS32 nt {};
    std::uintptr_t state = 0;
    std::uintptr_t realCamera = 0;
    std::uintptr_t internal = 0;
    const bool valid = readValue(process, base, dos)
        && dos.e_magic == IMAGE_DOS_SIGNATURE
        && dos.e_lfanew > 0
        && readValue(process, base + static_cast<std::uintptr_t>(dos.e_lfanew), nt)
        && nt.Signature == IMAGE_NT_SIGNATURE
        && nt.FileHeader.TimeDateStamp == moLiXiuImageTimestamp
        && nt.OptionalHeader.SizeOfImage == moLiXiuImageSize
        && readValue(process, base + moLiXiuSharedDataRva + sharedDataPointerOffset, state)
        && readValue(process, state, realCamera)
        && readValue(process, realCamera, internal)
        && readLegacyWString(process, internal + currentDeviceOffset, output);
    CloseHandle(process);
    return valid;
}

bool writeState(DWORD pid, const std::wstring& device)
{
    wchar_t appData[32768] {};
    const auto appDataLength = GetEnvironmentVariableW(
        L"APPDATA", appData, static_cast<DWORD>(sizeof(appData) / sizeof(appData[0])));
    if (appDataLength == 0 || appDataLength >= sizeof(appData) / sizeof(appData[0])) return false;

    const auto directory = std::wstring(appData, appDataLength) + L"\\SonoBus";
    if (! CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const auto finalPath = directory + L"\\molixiu-camera.txt";
    const auto temporaryPath = finalPath + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

    const auto utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, device.data(),
                                                static_cast<int>(device.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return false;
    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, device.data(), static_cast<int>(device.size()),
                            utf8.data(), utf8Length, nullptr, nullptr) != utf8Length)
        return false;
    const auto contents = "pid=" + std::to_string(pid) + "\ndevice=" + utf8 + "\n";

    const auto file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
                       && written == contents.size();
    if (complete) FlushFileBuffers(file);
    CloseHandle(file);
    if (complete && MoveFileExW(temporaryPath.c_str(), finalPath.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temporaryPath.c_str());
    return false;
}

int attach(const wchar_t* dllPath)
{
    const auto pid = findProcess();
    if (pid == 0) return 3;
    std::wstring selectedCamera;
    // Polling the existing selection is enough for supported MoLiXiu builds and
    // avoids touching its code. The VST invokes this bridge again every 5 seconds.
    if (readSelectedCamera(pid, selectedCamera) && writeState(pid, selectedCamera)) return 0;
    if (dllPath == nullptr || *dllPath == L'\0') return 2;
    if (alreadyLoaded(pid)) return 0;

    const auto process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                                   | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) return 4;
    const auto bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    auto* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remotePath == nullptr)
    {
        CloseHandle(process);
        return 5;
    }
    SIZE_T written = 0;
    const bool copied = WriteProcessMemory(process, remotePath, dllPath, bytes, &written) && written == bytes;
    auto* loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const auto thread = copied && loadLibrary != nullptr
                      ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr)
                      : nullptr;
    if (thread == nullptr)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }
    const auto waitResult = WaitForSingleObject(thread, 10000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return waitResult == WAIT_OBJECT_0 && module != 0 ? 0 : 7;
}

bool readerSelfTest()
{
    unsigned char shortObject[24] {};
    const wchar_t shortValue[] = L"camera";
    const DWORD shortSize = 6;
    const DWORD shortCapacity = 7;
    std::memcpy(shortObject, shortValue, shortSize * sizeof(wchar_t));
    std::memcpy(shortObject + 16, &shortSize, sizeof(shortSize));
    std::memcpy(shortObject + 20, &shortCapacity, sizeof(shortCapacity));

    unsigned char longObject[24] {};
    const wchar_t longValue[] = L"virtual camera device";
    const DWORD longSize = 21;
    const DWORD longCapacity = longSize;
    const auto longPointer = reinterpret_cast<std::uintptr_t>(longValue);
    std::memcpy(longObject, &longPointer, sizeof(longPointer));
    std::memcpy(longObject + 16, &longSize, sizeof(longSize));
    std::memcpy(longObject + 20, &longCapacity, sizeof(longCapacity));

    std::wstring result;
    return readLegacyWString(GetCurrentProcess(), reinterpret_cast<std::uintptr_t>(shortObject), result)
        && result == shortValue
        && readLegacyWString(GetCurrentProcess(), reinterpret_cast<std::uintptr_t>(longObject), result)
        && result == longValue;
}

int selfTest(const wchar_t* dllPath)
{
    if (! readerSelfTest()) return 10;
    const auto module = dllPath != nullptr ? LoadLibraryW(dllPath) : nullptr;
    if (module == nullptr) return 8;
    auto procedure = GetProcAddress(module, "SonoBusMoLiXiuLayoutSelfTest");
    if (procedure == nullptr) procedure = GetProcAddress(module, "_SonoBusMoLiXiuLayoutSelfTest");
    const auto check = reinterpret_cast<int (__cdecl*)()>(procedure);
    return check != nullptr ? check() : 9;
}
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dllPath = nullptr;
    bool runSelfTest = false;
    for (int index = 1; index + 1 < argc; ++index)
        if (_wcsicmp(argv[index], L"--dll") == 0) dllPath = argv[index + 1];
    for (int index = 1; index < argc; ++index)
        if (_wcsicmp(argv[index], L"--self-test") == 0) runSelfTest = true;
    return runSelfTest ? selfTest(dllPath) : attach(dllPath);
}
