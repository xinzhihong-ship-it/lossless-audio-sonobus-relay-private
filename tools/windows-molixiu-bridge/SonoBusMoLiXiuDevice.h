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

inline bool isVirtualCameraDevice(std::wstring_view value) noexcept
{
    constexpr std::wstring_view names[] {
        L"yyanchorvcam", L"yyanchormulvcam", L"obs virtual camera",
        L"webcastmate virtualcamera", L"virtual camera"
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
