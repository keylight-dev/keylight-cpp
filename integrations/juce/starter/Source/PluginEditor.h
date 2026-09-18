#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// ---------------------------------------------------------------------------
// The licensing UI: a status line, a key field, and an activate button.
//
// Everything here runs on the message thread. The activate() callback is
// delivered back on the message thread too, so components can be touched
// directly with no manual marshalling.
// ---------------------------------------------------------------------------
class KeylightStarterEditor : public juce::AudioProcessorEditor
{
public:
    explicit KeylightStarterEditor(KeylightStarterProcessor&);
    ~KeylightStarterEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Called from the processor's onStateChanged, on the message thread.
    void refreshLicensingUI();

private:
    void activateButtonClicked();

    KeylightStarterProcessor& processor_;

    juce::Label       titleLabel_;
    juce::Label       statusLabel_;
    juce::TextEditor  keyField_;
    juce::TextButton  activateButton_ { "Activate" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeylightStarterEditor)
};
