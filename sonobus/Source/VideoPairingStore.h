// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
#pragma once

#include <JuceHeader.h>

namespace VideoPairingStore
{
    juce::String account(const juce::String& host, const juce::String& group, const juce::String& user);
    bool save(const juce::String& account, const juce::String& pairingId, const juce::MemoryBlock& key, juce::String& error);
    bool load(const juce::String& account, const juce::String& pairingId, juce::MemoryBlock& key);
    void remove(const juce::String& account, const juce::String& pairingId);
}
