// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <string>

namespace
{
constexpr wchar_t hookName[] = L"SonoBusMoLiXiuHook.dll";

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

bool alreadyLoaded(DWORD pid)
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry { sizeof(entry) };
    bool result = false;
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, hookName) == 0)
            {
                result = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

int attach(const wchar_t* dllPath)
{
    if (dllPath == nullptr || *dllPath == L'\0') return 2;
    const auto pid = findProcess();
    if (pid == 0) return 3;
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

int selfTest(const wchar_t* dllPath)
{
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
