// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2025

#pragma once

#include <JuceHeader.h>

#include "EffectsBaseView.h"
#include "SonoTextButton.h"
#include "SonobusPluginProcessor.h"

class VideoRelayView final : public EffectsBaseView,
                             private juce::Timer
{
public:
    explicit VideoRelayView(SonobusAudioProcessor& processor_) : processor(processor_)
    {
        titleLabel.setFont(16.0f);
        titleLabel.setText(TRANS("自建视频中转"), dontSendNotification);
        titleLabel.setJustificationType(Justification::centred);
        titleLabel.setAccessible(false);

        infoLabel.setFont(13.0f);
        infoLabel.setJustificationType(Justification::centred);

        statusLabel.setFont(13.0f);
        statusLabel.setJustificationType(Justification::centredLeft);

        cameraLabel.setText(TRANS("摄像头:"), dontSendNotification);
        cameraLabel.setJustificationType(Justification::centredLeft);
        cameraBox.setTextWhenNothingSelected(TRANS("自动选择"));
        cameraBox.setTooltip(TRANS("优先使用上次选择的摄像头；设备不可用时自动回退。"));
        cameraBox.onChange = [this]
        {
            const auto selected = cameraBox.getSelectedId() - 1;
            if (isPositiveAndBelow(selected, cameraDevices.size()))
                processor.getVideoLinkInfo().cameraDevice = cameraDevices[selected];
            else
                processor.getVideoLinkInfo().cameraDevice.clear();
            processor.restartVideoSender();
            updateState();
        };
        refreshCameraList();

        autoConnectButton.setButtonText(TRANS("自动连接视频"));
        autoConnectButton.setTooltip(TRANS("SonoBus 加入群组后自动连接摄像头和视频中转服务。"));
        autoConnectButton.onClick = [this]
        {
            processor.getVideoLinkInfo().autoConnect = autoConnectButton.getToggleState();
            if (processor.getVideoLinkInfo().autoConnect)
                processor.restartVideoSender();
            else
                processor.stopVideoSender();
            updateState();
        };

        openButton.setButtonText(TRANS("在浏览器打开视频"));
        openButton.setColour(TextButton::buttonColourId, Colour::fromFloatRGBA(0.1f, 0.4f, 0.6f, 0.6f));
        openButton.setColour(SonoTextButton::outlineColourId, Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.4f));
        openButton.onClick = [this]
        {
            generateURL().launchInDefaultBrowser();
        };

        enableButton.setVisible(false);
        dragButton.setVisible(false);

        addAndMakeVisible(titleLabel);
        addAndMakeVisible(infoLabel);
        addAndMakeVisible(statusLabel);
        addAndMakeVisible(cameraLabel);
        addAndMakeVisible(cameraBox);
        addAndMakeVisible(autoConnectButton);
        addAndMakeVisible(openButton);

        updateState();
        startTimerHz(2);
    }

    ~VideoRelayView() override
    {
        stopTimer();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        titleLabel.setBounds(bounds.removeFromTop(28));
        bounds.removeFromTop(6);
        infoLabel.setBounds(bounds.removeFromTop(38));
        bounds.removeFromTop(4);
        statusLabel.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(8);

        auto cameraRow = bounds.removeFromTop(28);
        cameraLabel.setBounds(cameraRow.removeFromLeft(72));
        cameraRow.removeFromLeft(6);
        cameraBox.setBounds(cameraRow.removeFromLeft(jmax(120, cameraRow.getWidth() - 140)));
        cameraRow.removeFromLeft(8);
        autoConnectButton.setBounds(cameraRow);
        bounds.removeFromTop(12);
        openButton.setBounds(bounds.removeFromTop(32));

        minBounds.setSize(420, 220);
    }

    void updateState()
    {
        const auto group = processor.getCurrentJoinedGroup();
        autoConnectButton.setToggleState(processor.getVideoLinkInfo().autoConnect, dontSendNotification);
        statusLabel.setText(TRANS("状态: ") + processor.getVideoStatusText(), dontSendNotification);
        const auto camera = processor.getVideoCameraName();
        if (camera.isNotEmpty())
            statusLabel.setText(TRANS("状态: ") + processor.getVideoStatusText() + " · " + camera, dontSendNotification);

        infoLabel.setText(
            group.isEmpty()
                ? TRANS("加入 SonoBus 群组后，视频发送端会自动连接。视频链路不采集音频。")
                : TRANS("摄像头由 SonoBus VST 自动发送；观看者可从服务器管理端打开视频。"),
            dontSendNotification);
    }

private:
    void timerCallback() override
    {
        updateState();
    }

    void refreshCameraList()
    {
        cameraDevices = VideoRelayClient::getCameraDevices();
        cameraBox.clear();

        if (cameraDevices.isEmpty())
        {
            cameraBox.addItem(TRANS("未检测到摄像头"), 1);
            cameraBox.setEnabled(false);
            return;
        }

        cameraBox.setEnabled(true);
        int selectedId = 0;
        const auto preferred = processor.getVideoLinkInfo().cameraDevice;
        for (int i = 0; i < cameraDevices.size(); ++i)
        {
            cameraBox.addItem(cameraDevices[i], i + 1);
            if (cameraDevices[i] == preferred)
                selectedId = i + 1;
        }
        cameraBox.setSelectedId(selectedId, dontSendNotification);
    }

    juce::URL generateURL() const
    {
        auto host = processor.getRelayServerHost().trim();
        if (host.isEmpty())
            host = "127.0.0.1";
        if (host.containsChar(':') && ! host.startsWithChar('['))
            host = "[" + host + "]";

        auto group = processor.getCurrentJoinedGroup().trim();
        if (group.isEmpty())
            group = "未加入群组";

        StringPairArray params;
        params.set("room", "SB_" + group);
        return URL("http://" + host + ":19090/video/view").withParameters(params);
    }

    SonobusAudioProcessor& processor;
    juce::StringArray cameraDevices;
    Label titleLabel;
    Label infoLabel;
    Label statusLabel;
    Label cameraLabel;
    ComboBox cameraBox;
    ToggleButton autoConnectButton;
    TextButton openButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRelayView)
};
