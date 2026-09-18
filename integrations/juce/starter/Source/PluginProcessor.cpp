#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Demo interruption timing. Long enough to evaluate the plugin properly,
    // short enough that nobody prints a mix with it.
    constexpr double kSecondsBetweenBursts = 45.0;
    constexpr double kBurstSeconds         = 0.35;
}

keylight::Config KeylightStarterProcessor::makeConfig()
{
    keylight::Config cfg;
    cfg.tenantId  = starter_config::kTenantId;
    cfg.productId = starter_config::kProductId;
    cfg.sdkKey    = starter_config::kSdkKey;

    cfg.trustedKeys = {
        { starter_config::kTrustedKeyId, starter_config::kTrustedKey }
    };

    cfg.maxOfflineDays = starter_config::kMaxOfflineDays;

    // Seeds only. The dashboard's values replace these on the first check.
    cfg.trialDurationDays = starter_config::kSeedTrialDurationDays;
    cfg.freeTierEnabled   = starter_config::kSeedFreeTierEnabled;

    cfg.appVersion = JucePlugin_VersionString;

    return cfg;
}

KeylightStarterProcessor::KeylightStarterProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input",   juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    licensing_ = std::make_unique<keylight::juce_integration::Licensing>(makeConfig());

    // Always fires on the message thread, so components are safe to touch
    // directly — no callAsync needed. Set before checkOnLaunch, or the first
    // transition can resolve before anyone is listening.
    licensing_->onStateChanged = [this](keylight::State)
    {
        if (auto* editor = dynamic_cast<KeylightStarterEditor*>(getActiveEditor()))
            editor->refreshLicensingUI();
    };

    // Verifies any cached lease offline first, then refreshes in the
    // background. Returns immediately — a DAW scanning plugins must never be
    // made to wait on a network call.
    licensing_->checkOnLaunch([this](keylight::Result<keylight::State> result)
    {
        if (! result.is_ok())
            return;

        // No license and no trial yet? Start the evaluation window. The length
        // comes from the dashboard, not from a constant in this file.
        if (result.value() == keylight::State::Invalid)
            licensing_->startTrial();
    });

    // Background revalidation, so a revoked or expired key is noticed without
    // the customer restarting their DAW.
    licensing_->startAutoValidation();
}

KeylightStarterProcessor::~KeylightStarterProcessor()
{
    // ~Licensing() joins its worker and stops auto-validation. Nothing to do.
}

void KeylightStarterProcessor::prepareToPlay(double sampleRate, int)
{
    samplesBetweenBursts_ = static_cast<int>(sampleRate * kSecondsBetweenBursts);
    burstLengthSamples_   = static_cast<int>(sampleRate * kBurstSeconds);
    demoSampleCounter_    = 0;
    burstSamplesLeft_     = 0;
}

void KeylightStarterProcessor::releaseResources() {}

void KeylightStarterProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // ── The entire audio-thread licensing check ────────────────────────────
    //
    // hasFeature() is one lock-free atomic load. No allocation, no mutex, no
    // filesystem, no JUCE object construction. Everything that could block
    // already happened on a background thread.
    const bool isPro = licensing_->hasFeature(starter_config::kProFeature);

    if (isPro)
    {
        applyProPath(buffer);
        return;
    }

    // Unlicensed: the plugin still works, it just interrupts itself. A demo
    // that refuses to make sound teaches nobody anything about your plugin.
    applyFreePath(buffer);
    applyDemoBurst(buffer);
}

void KeylightStarterProcessor::applyFreePath(juce::AudioBuffer<float>& buffer)
{
    // Replace with your own free-path DSP.
    buffer.applyGain(0.9f);
}

void KeylightStarterProcessor::applyProPath(juce::AudioBuffer<float>& buffer)
{
    // Replace with your own pro-path DSP.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* samples = buffer.getWritePointer(ch);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
            samples[n] = std::tanh(samples[n] * 2.0f);
    }
}

void KeylightStarterProcessor::applyDemoBurst(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();

    // Already inside a burst that started in a previous block.
    if (burstSamplesLeft_ > 0)
    {
        const int n = juce::jmin(burstSamplesLeft_, numSamples);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* samples = buffer.getWritePointer(ch);

            for (int i = 0; i < n; ++i)
                samples[i] = demoNoise_.nextFloat() * 0.25f - 0.125f;
        }

        burstSamplesLeft_ -= n;
        return;
    }

    demoSampleCounter_ += numSamples;

    if (demoSampleCounter_ >= samplesBetweenBursts_)
    {
        demoSampleCounter_ = 0;
        burstSamplesLeft_  = burstLengthSamples_;
    }
}

juce::AudioProcessorEditor* KeylightStarterProcessor::createEditor()
{
    return new KeylightStarterEditor(*this);
}

void KeylightStarterProcessor::getStateInformation(juce::MemoryBlock&)
{
    // A common second demo limitation: refuse to persist state unless licensed.
    // This runs on the message thread, so a non-atomic read would be fine here
    // too — but there is no reason not to use the same accessor.
    //
    //   if (! licensing_->hasFeature(starter_config::kProFeature))
    //       return;
}

void KeylightStarterProcessor::setStateInformation(const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KeylightStarterProcessor();
}
