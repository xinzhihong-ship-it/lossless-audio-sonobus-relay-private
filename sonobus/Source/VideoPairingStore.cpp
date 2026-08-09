// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
#if defined(__APPLE__)
 #include <Security/Security.h>
#elif defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <wincred.h>
#endif

#include "VideoPairingStore.h"

namespace
{
juce::String credentialName(const juce::String& account, const juce::String& pairingId)
{
    return "com.Sonosaurus.SonoBus.VideoControl/" + juce::SHA256((account + "\n" + pairingId).toUTF8()).toHexString();
}

#if JUCE_MAC
CFStringRef makeCFString(const juce::String& value)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, value.toRawUTF8(), kCFStringEncodingUTF8);
}

CFDictionaryRef makeQuery(const juce::String& name, bool returnData)
{
    auto account = makeCFString(name);
    const void* keys[] = { kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit };
    const void* values[] = { kSecClassGenericPassword, CFSTR("com.Sonosaurus.SonoBus.VideoControl"), account,
                             returnData ? kCFBooleanTrue : kCFBooleanFalse, kSecMatchLimitOne };
    auto query = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(account);
    return query;
}
#endif
}

juce::String VideoPairingStore::account(const juce::String& host, const juce::String& group, const juce::String& user)
{
    return host.trim().toLowerCase() + "\n" + group.trim() + "\n" + user.trim();
}

bool VideoPairingStore::save(const juce::String& accountName,
                             const juce::String& pairingId,
                             const juce::MemoryBlock& key,
                             juce::String& error)
{
    if (key.getSize() != 32 || pairingId.isEmpty())
    {
        error = TRANS("摄像头配对密钥无效");
        return false;
    }
    const auto name = credentialName(accountName, pairingId);
#if JUCE_MAC
    auto query = makeQuery(name, false);
    SecItemDelete(query);
    CFRelease(query);

    auto account = makeCFString(name);
    auto data = CFDataCreate(kCFAllocatorDefault, static_cast<const UInt8*>(key.getData()), static_cast<CFIndex>(key.getSize()));
    const void* keys[] = { kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData, kSecAttrAccessible };
    const void* values[] = { kSecClassGenericPassword, CFSTR("com.Sonosaurus.SonoBus.VideoControl"), account,
                             data, kSecAttrAccessibleAfterFirstUnlock };
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const auto result = SecItemAdd(attributes, nullptr);
    CFRelease(attributes);
    CFRelease(data);
    CFRelease(account);
    if (result != errSecSuccess)
    {
        error = TRANS("无法写入 macOS 钥匙串，错误 ") + juce::String(static_cast<int>(result));
        return false;
    }
    return true;
#elif JUCE_WINDOWS
    const auto target = name.toWideCharPointer();
    const auto userName = pairingId.toWideCharPointer();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.getAddress());
    credential.UserName = const_cast<LPWSTR>(userName.getAddress());
    credential.CredentialBlobSize = static_cast<DWORD>(key.getSize());
    credential.CredentialBlob = const_cast<LPBYTE>(static_cast<const BYTE*>(key.getData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&credential, 0) == FALSE)
    {
        error = TRANS("无法写入 Windows 凭据管理器，错误 ") + juce::String(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
#else
    juce::ignoreUnused(accountName, pairingId, key);
    error = TRANS("此平台不支持安全摄像头配对存储");
    return false;
#endif
}

bool VideoPairingStore::load(const juce::String& accountName,
                             const juce::String& pairingId,
                             juce::MemoryBlock& key)
{
    const auto name = credentialName(accountName, pairingId);
#if JUCE_MAC
    auto query = makeQuery(name, true);
    CFTypeRef result = nullptr;
    const auto status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || result == nullptr || CFGetTypeID(result) != CFDataGetTypeID())
    {
        if (result != nullptr) CFRelease(result);
        return false;
    }
    auto data = static_cast<CFDataRef>(result);
    key.replaceAll(CFDataGetBytePtr(data), static_cast<size_t>(CFDataGetLength(data)));
    CFRelease(result);
    return key.getSize() == 32;
#elif JUCE_WINDOWS
    PCREDENTIALW credential = nullptr;
    const auto target = name.toWideCharPointer();
    if (CredReadW(target.getAddress(), CRED_TYPE_GENERIC, 0, &credential) == FALSE || credential == nullptr)
        return false;
    key.replaceAll(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return key.getSize() == 32;
#else
    juce::ignoreUnused(accountName, pairingId, key);
    return false;
#endif
}

void VideoPairingStore::remove(const juce::String& accountName, const juce::String& pairingId)
{
    if (pairingId.isEmpty()) return;
    const auto name = credentialName(accountName, pairingId);
#if JUCE_MAC
    auto query = makeQuery(name, false);
    SecItemDelete(query);
    CFRelease(query);
#elif JUCE_WINDOWS
    const auto target = name.toWideCharPointer();
    CredDeleteW(target.getAddress(), CRED_TYPE_GENERIC, 0);
#else
    juce::ignoreUnused(accountName, pairingId);
#endif
}
