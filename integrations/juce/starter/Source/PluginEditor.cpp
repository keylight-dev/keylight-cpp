#include "PluginEditor.h"

KeylightStarterEditor::KeylightStarterEditor(KeylightStarterProcessor& p)
    : AudioProcessorEditor(&p), processor_(p)
{
    titleLabel_.setText("Keylight Starter", juce::dontSendNotification);
    titleLabel_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);

    statusLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel_);

    keyField_.setTextToShowWhenEmpty("XXXX-XXXX-XXXX-XXXX",
                                     juce::Colours::grey);
    keyField_.setJustification(juce::Justification::centred);
    addAndMakeVisible(keyField_);

    activateButton_.onClick = [this] { activateButtonClicked(); };
    addAndMakeVisible(activateButton_);

    // Show the right thing immediately rather than waiting for a state change
    // that may already have happened before this editor existed.
    refreshLicensingUI();

    setSize(420, 240);
}

void KeylightStarterEditor::refreshLicensingUI()
{
    auto& licensing = processor_.licensing();

    // Read once. The state can change between calls, and a UI built from two
    // different reads of it can contradict itself.
    const auto state = licensing.state();

    switch (state)
    {
        case keylight::State::Licensed:
            statusLabel_.setText("Licensed", juce::dontSendNotification);
            keyField_.setVisible(false);
            activateButton_.setVisible(false);
            break;

        case keylight::State::Trial:
            statusLabel_.setText("Trial — "
                                 + juce::String(licensing.trialDaysLeft())
                                 + " days left",
                                 juce::dontSendNotification);
            keyField_.setVisible(true);
            activateButton_.setVisible(true);
            break;

        case keylight::State::FreeTier:
            statusLabel_.setText("Free mode — enter a key to unlock Pro",
                                 juce::dontSendNotification);
            keyField_.setVisible(true);
            activateButton_.setVisible(true);
            break;

        case keylight::State::Expired:
            statusLabel_.setText("Trial ended — enter your key to unlock",
                                 juce::dontSendNotification);
            keyField_.setVisible(true);
            activateButton_.setVisible(true);
            break;

        case keylight::State::Limited:
            // The server could not mint a full lease, so this customer runs
            // degraded rather than locked out. Do not ask them to activate —
            // they already did, and telling them otherwise is alarming.
            statusLabel_.setText("Running in limited mode",
                                 juce::dontSendNotification);
            keyField_.setVisible(false);
            activateButton_.setVisible(false);
            break;

        case keylight::State::Invalid:
        default:
            statusLabel_.setText("Enter your license key",
                                 juce::dontSendNotification);
            keyField_.setVisible(true);
            activateButton_.setVisible(true);
            break;
    }

    resized();
}

void KeylightStarterEditor::activateButtonClicked()
{
    // Trim hard. A key pasted out of an email client arrives with trailing
    // whitespace and sometimes a line break, and "invalid key" is a terrible
    // thing to tell someone who typed it correctly.
    const auto key = keyField_.getText().trim();

    if (key.isEmpty())
        return;

    activateButton_.setEnabled(false);
    statusLabel_.setText("Activating…", juce::dontSendNotification);

    processor_.licensing().activate(key,
        [this](keylight::Result<keylight::State> result)
        {
            // Delivered on the message thread — components are safe to touch.
            activateButton_.setEnabled(true);

            if (result.is_ok() && result.value() == keylight::State::Licensed)
            {
                refreshLicensingUI();
                return;
            }

            const juce::String message = result.is_ok()
                ? "That key was not accepted. Check it and try again."
                : juce::String("Could not reach Keylight: ")
                      + result.error().message.c_str();

            statusLabel_.setText(message, juce::dontSendNotification);
        });
}

void KeylightStarterEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void KeylightStarterEditor::resized()
{
    auto area = getLocalBounds().reduced(24);

    titleLabel_.setBounds(area.removeFromTop(32));
    area.removeFromTop(8);
    statusLabel_.setBounds(area.removeFromTop(28));
    area.removeFromTop(16);

    if (keyField_.isVisible())
    {
        keyField_.setBounds(area.removeFromTop(32));
        area.removeFromTop(12);
        activateButton_.setBounds(area.removeFromTop(34).reduced(80, 0));
    }
}
