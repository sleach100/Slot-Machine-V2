#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

class SlotMachineAudioProcessor : public juce::AudioProcessor
{
public:

    void requestManualTrigger(int index);

    void clearSlot(int index, bool allowTail = false);
    void clearAllSlots();

    // ====== Constants ======
    static constexpr int kNumSlots = 16;
    static constexpr int kCountModeBaseBeats = 4;

    // ====== Construction ======
    SlotMachineAudioProcessor();
    ~SlotMachineAudioProcessor() override;

    // ====== AudioProcessor standard ======
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        juce::ignoreUnused(layouts);
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
            && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
        if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
            return false;
#endif
#endif
        return true;
    }

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Programs (unused)
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override
    {
#if JucePlugin_WantsMidiInput
        return true;
#else
        return false;
#endif
    }
    bool producesMidi() const override
    {
#if JucePlugin_ProducesMidiOutput
        return true;
#else
        return false;
#endif
    }
    bool isMidiEffect() const override
    {
#if JucePlugin_IsMidiEffect
        return true;
#else
        return false;
#endif
    }

    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // State
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ====== APVTS ======
    using APVTS = juce::AudioProcessorValueTreeState;
    APVTS apvts{ *this, nullptr, "PARAMETERS", createParameterLayout() };
    static APVTS::ParameterLayout createParameterLayout();

    // ====== Editor helpers ======
    void resetAllPhases(bool immediate);
    bool        slotHasSample(int index) const;
    juce::String getSlotFilePath(int index) const;
    void        setSlotFilePath(int index, const juce::String& path);
    bool        loadSampleForSlot(int index, const juce::File& f, bool allowTail = false);
    void        upgradeLegacySlotParameters();
    juce::ValueTree copyStateWithVersion();
    void initialiseStateForFirstEditor();
    bool consumeInitialiseOnFirstEditor();

    // UI polling
    uint32_t getSlotHitCounter(int index) const;
    double   getSlotPhase(int index) const;
    double getMasterPhase() const;
    bool exportAudioCycles(const juce::File& file, int cyclesToExport, juce::String& errorMessage);

    // Count beat masks
    uint64_t getSlotCountMask(int index) const;
    void     setSlotCountMask(int index, uint64_t mask);
    static uint64_t maskForBeats(int beats);

    // Pattern management
    juce::ValueTree getPatternsTree();
    juce::ValueTree createDefaultPatternTree(const juce::String& name) const;
    juce::ValueTree createPatternTreeFromCurrentState(const juce::String& name) const;
    void storeCurrentStateInPattern(juce::ValueTree pattern) const;
    void applyPatternTree(const juce::ValueTree& pattern, juce::Array<int>* failedSlots = nullptr, bool allowTailRelease = false);
    void setCurrentPatternIndex(int index);
    int  getCurrentPatternIndex() const;

private:
    std::array<std::atomic<int>, kNumSlots> pendingManualTriggers;
    std::array<std::atomic<uint64_t>, kNumSlots> countBeatMasks{};
    double currentCycleBeats = 1.0;
    double currentCyclePhase01 = 0.0;

    // ====== Internal per-slot voice ======
    struct SlotVoice
    {
        double framesPerPeriodCached = 0.0; // cached period in frames

        juce::AudioBuffer<float> sample;     // mono duplicated to stereo
        juce::AudioBuffer<float> tailSample; // retains previous sample while tail rings
        double sampleRate = 44100.0;
        double phase = 0.0;   // 0..1 visual phase over its own period
        double framesUntilHit = 0.0;   // countdown to next trigger
        float  panL = 0.7071f;
        float  panR = 0.7071f;
        bool   active = false; // has sample
        uint32_t hitCounter = 0;

        // playback
        int playIndex = -1; // -1 idle
        int playLength = 0;

        // tail playback (for seamless pattern switches)
        int tailIndex = -1;
        int tailLength = 0;
        float tailEnv = 0.0f;
        float tailEnvAlpha = 1.0f;
        int   tailEnvSamplesElapsed = 0;
        int   tailEnvMaxSamples = 0;
        float tailPanL = 0.7071f;
        float tailPanR = 0.7071f;
        bool  tailActive = false;

        // persistence
        juce::String filePath;

        // === Decay envelope (NEW) ===
        float env = 0.0f;
        float envAlpha = 1.0f;
        int   envSamplesElapsed = 0;
        int   envMaxSamples = 0;

        void prepare(double sr);
        void resetPhase(bool hard);
        void setPan(float panMinus1to1);
        void setDecayMs(float ms);

        //--------------------------
        void onPeriodChange(double newFramesPerPeriod) noexcept
        {
            if (newFramesPerPeriod <= 0.0)
            {
                framesPerPeriodCached = 0.0;
                return;
            }

            if (framesPerPeriodCached > 0.0)
            {
                const double scale = newFramesPerPeriod / framesPerPeriodCached;
                framesUntilHit *= scale;
            }
            else
            {
                framesUntilHit = newFramesPerPeriod * (1.0 - phase);
            }

            framesPerPeriodCached = newFramesPerPeriod;
        }

        //--------------------------

        void loadFile(const juce::File& f);
        void trigger();
        void mixInto(juce::AudioBuffer<float>& io, int numSamples, float gain);

        bool hasSample() const { return active && sample.getNumSamples() > 0; }
        void setFilePath(const juce::String& s) { filePath = s; }
        juce::String getFilePath() const { return filePath; }

        void clear(bool allowTail) noexcept;

    };

    std::array<SlotVoice, kNumSlots> slots;
    double currentSampleRate = 44100.0;
    //-------------------
    double masterBeatsAccum = 0.0; // total beats elapsed while running (not modulo)

    bool initialiseOnFirstEditor = true;

    void refreshSlotCountMasksFromState();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotMachineAudioProcessor)
};
