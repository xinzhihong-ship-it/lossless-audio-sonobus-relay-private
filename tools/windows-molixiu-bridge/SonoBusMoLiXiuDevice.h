// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <cstddef>
#include <string_view>

namespace sonobus::molixiu
{
inline wchar_t lowerAscii(wchar_t value) noexcept
{
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

inline bool containsInsensitive(std::wstring_view value, std::wstring_view token) noexcept
{
    if (token.empty() || value.size() < token.size()) return false;
    for (size_t offset = 0; offset + token.size() <= value.size(); ++offset)
    {
        size_t index = 0;
        for (; index < token.size(); ++index)
            if (lowerAscii(value[offset + index]) != lowerAscii(token[index])) break;
        if (index == token.size()) return true;
    }
    return false;
}

inline bool equalsInsensitive(std::wstring_view left, std::wstring_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (lowerAscii(left[index]) != lowerAscii(right[index])) return false;
    return true;
}

inline bool startsWithInsensitive(std::wstring_view value, std::wstring_view prefix) noexcept
{
    return value.size() >= prefix.size()
        && equalsInsensitive(value.substr(0, prefix.size()), prefix);
}

inline std::wstring_view sourceGroupDeviceKey(std::wstring_view value) noexcept
{
    constexpr std::wstring_view prefixes[] {
        L"@device:pnp:", L"@device_pnp_", L"dshow:"
    };
    bool stripped = true;
    while (stripped)
    {
        stripped = false;
        for (const auto prefix : prefixes)
            if (startsWithInsensitive(value, prefix))
            {
                value = value.substr(prefix.size());
                stripped = true;
                break;
            }
    }
    const auto interfaceGuid = value.find(L"#{");
    return interfaceGuid == std::wstring_view::npos ? value : value.substr(0, interfaceGuid);
}

inline bool samePhysicalCameraSourceGroup(std::wstring_view hint, std::wstring_view sourceGroup) noexcept
{
    const auto hintKey = sourceGroupDeviceKey(hint);
    const auto sourceKey = sourceGroupDeviceKey(sourceGroup);
    return ! hintKey.empty() && ! sourceKey.empty() && equalsInsensitive(hintKey, sourceKey);
}

inline bool isVirtualCameraDevice(std::wstring_view value) noexcept
{
    constexpr std::wstring_view names[] {
        L"yyanchorvcam", L"yyanchormulvcam", L"yyanchormulcam",
        L"obs virtual camera", L"webcastmate virtualcamera", L"virtual camera",
        L"yy\u5F00\u64AD", L"\u9B54\u529B\u79C0"
    };
    for (const auto name : names)
        if (containsInsensitive(value, name)) return true;
    return false;
}

inline bool shouldPersistCameraHint(std::wstring_view value) noexcept
{
    return ! value.empty() && ! isVirtualCameraDevice(value);
}
}
