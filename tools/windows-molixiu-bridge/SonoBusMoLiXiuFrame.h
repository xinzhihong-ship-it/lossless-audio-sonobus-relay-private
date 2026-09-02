// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <cstdint>

namespace sonobus::molixiu
{
constexpr wchar_t kFrameMappingName[] = L"Local\\SonoBusMoLiXiuFrame";
constexpr std::uint32_t kFrameMagic = 0x53424D46; // SBMF
constexpr std::uint32_t kFrameVersion = 1;
constexpr std::uint32_t kPixelBgr24 = 1;
constexpr std::uint32_t kPixelBgra32 = 2;
constexpr std::uint32_t kPixelNv12 = 3;
constexpr std::uint32_t kPixelYuy2 = 4;
constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

struct FrameHeader
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t pixelFormat = 0;
    std::uint32_t bytes = 0;
    std::uint32_t frameNumber = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(FrameHeader) == 40, "Unexpected MoLiXiu frame header layout.");
constexpr std::uint32_t kFrameMappingBytes = sizeof(FrameHeader) + kMaxFrameBytes;
}
