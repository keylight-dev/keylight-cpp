#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

#include "KeylightJuce.h"
#include "KeylightConfig.h"

// ---------------------------------------------------------------------------
// A minimal but complete licensed plugin.
//
// The processing is deliberately trivial — a gain trim on the free path and a
// soft-clip drive on the pro path — so the licensing structure is the only
// thing worth reading here. Replace applyFreePath/applyProPath with your own
// DSP and the licensing wiring stays exactly as it is.
// ---------------------------------------------------------------------------
class KeylightStarterProcessor : public juce::AudioProcessor
{
public:
    KeylightStarterProcessor();
    ~KeylightStarterProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Keylight Starter"; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // The editor drives activation and reads state through this.
    keylight::juce_integration::Licensing& licensing() { return *licensing_; }

private:
    void applyFreePath(juce::AudioBuffer<float>&);
    void applyProPath(juce::AudioBuffer<float>&);
    void applyDemoBurst(juce::AudioBuffer<float>&);

    static keylight::Config makeConfig();

    // Owns the Client, the JUCE transport, the on-disk lease store, and the
    // lock-free atomic that processBlock reads. Held by unique_ptr because
    // Licensing owns a thread and is deliberately neither copyable nor
    // movable — this also lets us build the Config fully before constructing.
    std::unique_ptr<keylight::juce_integration::Licensing> licensing_;

    // ── Demo-mode state. Audio thread only. ────────────────────────────────
    // Counted in samples rather than seconds: no clock call on the audio
    // thread, and the burst interval stays correct at any sample rate.
    int demoSampleCounter_   = 0;
    int samplesBetweenBursts_ = 0;
    int burstLengthSamples_   = 0;
    int burstSamplesLeft_     = 0;

    juce::Random demoNoise_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeylightStarterProcessor)
};
