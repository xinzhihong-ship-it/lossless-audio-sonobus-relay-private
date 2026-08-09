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
        titleLabel.setText(TRANS("H.264 / WebRTC 视频"), dontSendNotification);
        titleLabel.setJustificationType(Justification::centred);
        titleLabel.setAccessible(false);

        infoLabel.setFont(13.0f);
        infoLabel.setJustificationType(Justification::centred);
        infoLabel.setText(TRANS("首次输入管理员后台生成的配对信息。之后只有管理员能开关摄像头和选择设备。"), dontSendNotification);

        statusLabel.setFont(13.0f);
        statusLabel.setJustificationType(Justification::centredLeft);

        pairingLabel.setText(TRANS("配对信息:"), dontSendNotification);
        pairingLabel.setJustificationType(Justification::centredLeft);
        pairingEditor.setPasswordCharacter(0x2022);
        pairingEditor.setTextToShowWhenEmpty(TRANS("SBPAIR1...（仅首次配对）"), Colours::grey);
        pairingEditor.setTooltip(TRANS("配对密钥只写入 macOS 钥匙串或 Windows 凭据管理器，不写入 DAW 工程。"));

        savePairingButton.setButtonText(TRANS("保存配对"));
        savePairingButton.onClick = [this]
        {
            String error;
            if (processor.setVideoPairingText(pairingEditor.getText(), error))
            {
                pairingEditor.clear();
                localMessage = TRANS("配对已安全保存；等待管理员控制。 ");
            }
            else
            {
                localMessage = error;
            }
            updateState();
        };

        revokePairingButton.setButtonText(TRANS("撤销本机配对"));
        revokePairingButton.setColour(TextButton::buttonColourId, Colours::darkred.withAlpha(0.7f));
        revokePairingButton.onClick = [this]
        {
            processor.clearVideoPairing();
            pairingEditor.clear();
            localMessage = TRANS("本机配对已撤销，管理员无法再开启此摄像头。 ");
            updateState();
        };

        openButton.setButtonText(TRANS("在浏览器打开群组视频"));
        openButton.setColour(TextButton::buttonColourId, Colour::fromFloatRGBA(0.1f, 0.4f, 0.6f, 0.6f));
        openButton.setColour(SonoTextButton::outlineColourId, Colour::fromFloatRGBA(0.5f, 0.5f, 0.5f, 0.4f));
        openButton.onClick = [this] { generateURL().launchInDefaultBrowser(); };

        enableButton.setVisible(false);
        dragButton.setVisible(false);
        addAndMakeVisible(titleLabel);
        addAndMakeVisible(infoLabel);
        addAndMakeVisible(statusLabel);
        addAndMakeVisible(pairingLabel);
        addAndMakeVisible(pairingEditor);
        addAndMakeVisible(savePairingButton);
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
        infoLabel.setBounds(bounds.removeFromTop(42));
        bounds.removeFromTop(4);
        statusLabel.setBounds(bounds.removeFromTop(42));
        bounds.removeFromTop(8);

        auto pairingRow = bounds.removeFromTop(30);
        pairingLabel.setBounds(pairingRow.removeFromLeft(72));
        pairingRow.removeFromLeft(6);
        savePairingButton.setBounds(pairingRow.removeFromRight(88));
        pairingRow.removeFromRight(6);
        pairingEditor.setBounds(pairingRow);
        bounds.removeFromTop(10);

        auto actionRow = bounds.removeFromTop(32);
        revokePairingButton.setBounds(actionRow.removeFromLeft(132));
        actionRow.removeFromLeft(8);
        openButton.setBounds(actionRow);
        minBounds.setSize(500, 230);
    }

private:
    void timerCallback() override { updateState(); }

public:
    void updateState()
    {
        const auto group = processor.getCurrentJoinedGroup();
        const auto paired = processor.hasVideoPairing();
        if (!paired && !group.isEmpty() && isShowing() && pairingEditor.getText().isEmpty())
        {
            const auto clipboardText = juce::SystemClipboard::getTextFromClipboard().trim();
            String clipboardPairingId;
            MemoryBlock clipboardKey;
            if (VideoRelayClient::parsePairingText(clipboardText, clipboardPairingId, clipboardKey))
            {
                pairingEditor.setText(clipboardText, dontSendNotification);
                localMessage = TRANS("已从剪贴板自动填入配对信息，请点击保存配对。");
            }
        }

        auto statusText = TRANS("状态: ") + processor.getVideoStatusText();
        const auto camera = processor.getVideoCameraName();
        if (camera.isNotEmpty()) statusText += " · " + camera;
        if (localMessage.isNotEmpty()) statusText += "\n" + localMessage;
        statusLabel.setText(statusText, dontSendNotification);

        savePairingButton.setEnabled(!group.isEmpty() && pairingEditor.getText().isNotEmpty());
        revokePairingButton.setEnabled(paired);
        openButton.setEnabled(!group.isEmpty());
        pairingEditor.setEnabled(!group.isEmpty());
        pairingEditor.setTextToShowWhenEmpty(
            group.isEmpty() ? TRANS("请先加入 SonoBus 群组")
                            : paired ? TRANS("已配对；输入新信息可重新配对") : TRANS("粘贴管理员生成的 SBPAIR1..."),
            Colours::grey);
    }

private:
    juce::URL generateURL() const
    {
        auto host = processor.getRelayServerHost().trim();
        if (host.isEmpty()) host = "127.0.0.1";
        if (host.containsChar(':') && !host.startsWithChar('[')) host = "[" + host + "]";
        auto group = processor.getCurrentJoinedGroup().trim();
        if (group.isEmpty()) group = "未加入群组";
        return URL("http://" + host + ":19090/" + URL::addEscapeChars("SB_" + group, false));
    }

    SonobusAudioProcessor& processor;
    String localMessage;
    Label titleLabel;
    Label infoLabel;
    Label statusLabel;
    Label pairingLabel;
    TextEditor pairingEditor;
    TextButton savePairingButton;
    TextButton revokePairingButton;
    TextButton openButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRelayView)
};
