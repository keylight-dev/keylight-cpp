/*
 * PluginProcessor.cpp — Keylight licensing integration snippet for a JUCE plugin
 *
 * This is a SHORT reference snippet, not a complete plugin.  It shows:
 *   1. A Licensing member in the AudioProcessor.
 *   2. processBlock reading hasFeature("pro") from the atomic — no lock/alloc.
 *   3. The editor calling activate() with a callback on the message thread.
 *
 * In your real plugin, merge these patterns into your existing processor files.
 */

// ---------------------------------------------------------------------------
// PluginProcessor.h (excerpt)
// ---------------------------------------------------------------------------
//
//  #include "KeylightJuce.h"           // single-header drop-in
//
//  class MyAudioProcessor : public juce::AudioProcessor
//  {
//  public:
//      MyAudioProcessor();
//      ~MyAudioProcessor() override;
//
//      void prepareToPlay(double, int) override {}
//      void releaseResources()         override {}
//      void processBlock(juce::AudioBuffer<float>& buffer,
//                        juce::MidiBuffer&         midi) override;
//
//      // Expose to the editor for activate/deactivate UI.
//      keylight::juce_integration::Licensing& licensing() { return *licensing_; }
//
//  private:
//      // Licensing member — owns transport, store, and Client.
//      // Unique_ptr so we can fully construct Config before creating it.
//      std::unique_ptr<keylight::juce_integration::Licensing> licensing_;
//
//      // ... rest of your processor ...
//  };

// ---------------------------------------------------------------------------
// PluginProcessor.cpp
// ---------------------------------------------------------------------------

#include "PluginProcessor.h"

MyAudioProcessor::MyAudioProcessor()
{
    // ── Configure Keylight ─────────────────────────────────────────────────
    keylight::Config cfg;
    cfg.tenantId   = "your-tenant-id";          // from the Keylight dashboard
    cfg.productId  = "your-product-id";
    cfg.trustedKeys = {
        { "kid-1", "<base64-Ed25519-public-key>" } // from the dashboard
    };
    cfg.maxOfflineDays = 7;
    // cfg.appVersion = "1.0.0";  // optional telemetry

    // ── Construct the licensing wrapper ───────────────────────────────────
    // storePath left empty → defaults to
    //   macOS: ~/Library/Application Support/Keylight/<tenant>-<product>.lease
    //   Win:   %APPDATA%\Keylight\<tenant>-<product>.lease
    licensing_ = std::make_unique<keylight::juce_integration::Licensing>(cfg);

    // Optional: called on every state change (message thread).
    licensing_->onStateChanged = [this](keylight::State newState)
    {
        // Trigger an editor repaint, update UI labels, etc.
        // This fires on the message thread — safe to touch JUCE components.
        (void)newState; // replace with your actual UI update
    };

    // ── Run checkOnLaunch in the background ───────────────────────────────
    // Reads the cached lease from disk, then hits the network if stale.
    // Result (and onStateChanged) delivered to the message thread.
    licensing_->checkOnLaunch([](keylight::Result<keylight::State> result)
    {
        if (result.is_ok())
        {
            // Initial state known.  The Licensing wrapper also fires
            // onStateChanged for any subsequent transitions.
        }
    });

    // Optional: start background auto-revalidation every 30 min.
    // licensing_->startAutoValidation();
}

MyAudioProcessor::~MyAudioProcessor()
{
    // licensing_ destructor joins the background thread and stops
    // auto-validation automatically.  Nothing extra needed here.
}

void MyAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer&          /*midi*/)
{
    // ── AUDIO-THREAD FEATURE GATE ─────────────────────────────────────────
    //
    // hasFeature("pro") reads a std::atomic<bool> — no lock, no allocation,
    // no heap access, no JUCE class construction.  This is safe on the real-
    // time thread.
    //
    // The atomic is updated by the Licensing subscription callback which runs
    // on the background network thread after every state change and posts to
    // the message thread via callAsync.  The audio thread never touches
    // the SDK's internal mutex or networking paths.
    //
    if (!licensing_->hasFeature("pro"))
    {
        // Free tier: mute output or apply a limitation.
        buffer.clear();
        return;
    }

    // Pro tier processing ...
}

// ---------------------------------------------------------------------------
// PluginEditor.cpp (excerpt — activation from the UI)
// ---------------------------------------------------------------------------
//
// void MyAudioProcessorEditor::activateButtonClicked()
// {
//     juce::String key = licenseKeyField.getText().trim();
//     if (key.isEmpty()) return;
//
//     activateButton.setEnabled(false);
//     statusLabel.setText("Activating...", juce::dontSendNotification);
//
//     // activate() dispatches to a background thread and delivers the callback
//     // back on the message thread — safe to touch UI components here.
//     processor.licensing().activate(key,
//         [this](keylight::Result<keylight::State> result)
//         {
//             activateButton.setEnabled(true);
//             if (result.is_ok() && result.value() == keylight::State::Licensed)
//             {
//                 statusLabel.setText("Licensed!", juce::dontSendNotification);
//             }
//             else
//             {
//                 juce::String msg = result.is_ok()
//                     ? "Activation failed: invalid key"
//                     : juce::String("Error: ") + result.error().message.c_str();
//                 statusLabel.setText(msg, juce::dontSendNotification);
//             }
//         });
// }
