// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <JuceHeader.h>

#include "EffectsBaseView.h"
#include "SonoTextButton.h"
#include "SonobusPluginProcessor.h"
#include "VideoRelaySupport.h"

class VideoRelayView final : public EffectsBaseView,
                             private juce::Timer
{
public:
    explicit VideoRelayView(SonobusAudioProcessor& processor_) : processor(processor_)
    {
        titleLabel.setFont(16.0f);
        titleLabel.setText(sonobus::video::translated(u8"H.264 / WebRTC 视频"), dontSendNotification);
        titleLabel.setJustificationType(Justification::centred);
        titleLabel.setAccessible(false);

        infoLabel.setFont(13.0f);
        infoLabel.setJustificationType(Justification::centred);
        infoLabel.setText(sonobus::video::translated(u8"客户端会自动申请授权；摄像头开关和设备选择仅由后台管理员控制。"), dontSendNotification);

        statusLabel.setFont(13.0f);
        statusLabel.setJustificationType(Justification::centredLeft);

        revokePairingButton.setButtonText(sonobus::video::translated(u8"撤销本机授权"));
        revokePairingButton.setColour(TextButton::buttonColourId, Colours::darkred.withAlpha(0.7f));
        revokePairingButton.onClick = [this]
        {
            processor.clearVideoPairing();
            localMessage = sonobus::video::translated(u8"本机授权已撤销；后台重新批准后才会恢复。");
            updateState();
        };

        openButton.setButtonText(sonobus::video::translated(u8"在浏览器打开群组视频"));
        openButton.setColour(TextButton::buttonColourId, Colour::fromFloatRGBA(0.1f, 0.4f, 0.6f, 0.6f));
        openButton.setColour(SonoTextButton::outlineColourId, Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.4f));
        openButton.onClick = [this] { generateURL().launchInDefaultBrowser(); };

        enableButton.setVisible(false);
        dragButton.setVisible(false);
        addAndMakeVisible(titleLabel);
        addAndMakeVisible(infoLabel);
        addAndMakeVisible(statusLabel);
        addAndMakeVisible(revokePairingButton);
        addAndMakeVisible(openButton);

        updateState();
        startTimerHz(2);
    }

    ~VideoRelayView() override { stopTimer(); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        titleLabel.setBounds(bounds.removeFromTop(28));
        bounds.removeFromTop(6);
        infoLabel.setBounds(bounds.removeFromTop(36));
        bounds.removeFromTop(6);
        statusLabel.setBounds(bounds.removeFromTop(50));
        bounds.removeFromTop(8);

        auto actionRow = bounds.removeFromTop(32);
        revokePairingButton.setBounds(actionRow.removeFromLeft(132));
        actionRow.removeFromLeft(8);
        openButton.setBounds(actionRow);
        minBounds.setSize(500, 190);
    }

    void updateState()
    {
        const auto group = processor.getCurrentJoinedGroup();
        auto statusText = sonobus::video::translated(u8"状态：") + processor.getVideoStatusText();
        const auto camera = processor.getVideoCameraName();
        if (camera.isNotEmpty()) statusText += sonobus::video::utf8(u8" · ") + camera;
        if (localMessage.isNotEmpty()) statusText += "\n" + localMessage;
        statusLabel.setText(statusText, dontSendNotification);

        revokePairingButton.setEnabled(processor.hasVideoPairing());
        openButton.setEnabled(!group.isEmpty());
    }

private:
    void timerCallback() override { updateState(); }

    juce::URL generateURL() const
    {
        auto host = processor.getRelayServerHost().trim();
        if (host.isEmpty()) host = "127.0.0.1";
        if (host.containsChar(':') && !host.startsWithChar('[')) host = "[" + host + "]";
        auto group = processor.getCurrentJoinedGroup().trim();
        if (group.isEmpty()) group = sonobus::video::translated(u8"未加入群组");
        return URL("http://" + host + ":19090/" + URL::addEscapeChars("SB_" + group, false));
    }

    SonobusAudioProcessor& processor;
    String localMessage;
    Label titleLabel;
    Label infoLabel;
    Label statusLabel;
    TextButton revokePairingButton;
    TextButton openButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRelayView)
};
