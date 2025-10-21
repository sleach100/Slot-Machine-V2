#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <limits>
#include <mutex>

using APVTS = juce::AudioProcessorValueTreeState;

//==============================================================================
// 
namespace {
static int igcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a < 0 ? -a : a; }
static int ilcm(int a, int b) { return (a == 0 || b == 0) ? 0 : (a / igcd(a, b)) * b; }

static void accumulateCycleLength(int spacingNumerator, int spacingDenominator,
    int& cycleNumerator, int& cycleDenominator, bool& hasCycle)
{
    if (spacingNumerator <= 0 || spacingDenominator <= 0)
        return;

    const int spacingGcd = igcd(spacingNumerator, spacingDenominator);
    spacingNumerator /= spacingGcd;
    spacingDenominator /= spacingGcd;

    if (!hasCycle)
    {
        cycleNumerator = spacingNumerator;
        cycleDenominator = spacingDenominator;
        hasCycle = true;
        return;
    }

    cycleNumerator = ilcm(cycleNumerator, spacingNumerator);
    cycleDenominator = igcd(cycleDenominator, spacingDenominator);

    const int reduce = igcd(cycleNumerator, cycleDenominator);
    if (reduce != 0)
    {
        cycleNumerator /= reduce;
        cycleDenominator /= reduce;
    }
}

static constexpr float kDecayUiMin = 1.0f;
static constexpr float kDecayUiMax = 100.0f;
static constexpr float kDecayUiStep = 0.1f;
static constexpr float kDecayUiSkew = 0.4f;

static constexpr float kDecayMsMin = 10.0f;
static constexpr float kDecayMsMax = 4000.0f;

static const juce::Identifier kStateVersionProperty("slotMachineStateVersion");
static const juce::Identifier kAutoInitialiseProperty("slotMachineAutoInitialise");
static const juce::Identifier kPatternsNodeId("patterns");
static const juce::Identifier kPatternNodeType("pattern");
static const juce::Identifier kPatternNameProperty("name");
static const juce::Identifier kPatternMasterBpmProperty("masterBPM");
static const juce::Identifier kPatternTimingModeProperty("timingMode");
static const juce::Identifier kCurrentPatternIndexProperty("currentPatternIndex");
static constexpr int kCurrentStateVersion = 4;

static juce::AudioFormatManager& getSharedFormatManager()
{
    static juce::AudioFormatManager fm;
    static std::once_flag once;
    std::call_once(once, [] { fm.registerBasicFormats(); });
    return fm;
}

static std::unique_ptr<juce::AudioFormatReader> createReaderForEmbeddedSample(const EmbeddedSamples::SampleInfo& sample)
{
    int dataSize = 0;
    const char* data = BinaryData::getNamedResource(sample.resourceName.toRawUTF8(), dataSize);
    if (data == nullptr || dataSize <= 0)
        return nullptr;

    std::unique_ptr<juce::InputStream> stream = std::make_unique<juce::MemoryInputStream>(data, (size_t)dataSize, false);
    return std::unique_ptr<juce::AudioFormatReader>(getSharedFormatManager().createReaderFor(std::move(stream)));
}

static juce::String makeEmbeddedPath(const EmbeddedSamples::SampleInfo& sample)
{
    return "embedded:" + sample.originalFilename;
}

static bool isEmbeddedSamplePath(const juce::String& path)
{
    return path.startsWithIgnoreCase("embedded:");
}

static juce::String extractEmbeddedFilename(const juce::String& path)
{
    return path.fromFirstOccurrenceOf(":", false, false).trim();
}

static bool varToFloat(const juce::var& value, float& out)
{
    if (value.isDouble() || value.isInt())
    {
        out = (float)value;
        return true;
    }

    if (value.isString())
    {
        const auto text = value.toString();
        if (text.isNotEmpty())
        {
            out = text.getFloatValue();
            return true;
        }
    }

    return false;
}

static float decayUiToMilliseconds(float uiValue)
{
    static const juce::NormalisableRange<float> uiRange(kDecayUiMin, kDecayUiMax, kDecayUiStep, kDecayUiSkew);
    static const juce::NormalisableRange<float> msRange(kDecayMsMin, kDecayMsMax, 1.0f, kDecayUiSkew);

    const float clamped = juce::jlimit(kDecayUiMin, kDecayUiMax, uiValue);
    const float normalised = uiRange.convertTo0to1(clamped);
    return msRange.convertFrom0to1(normalised);
}

static float legacyDecayMsToUi(float msValue)
{
    static const juce::NormalisableRange<float> uiRange(kDecayUiMin, kDecayUiMax, kDecayUiStep, kDecayUiSkew);
    static const juce::NormalisableRange<float> msRange(kDecayMsMin, kDecayMsMax, 1.0f, kDecayUiSkew);

    const float clamped = juce::jlimit(kDecayMsMin, kDecayMsMax, msValue);
    const float normalised = msRange.convertTo0to1(clamped);
    return uiRange.convertFrom0to1(normalised);
}

static void approximateRational(double x, int maxDen, int& num, int& den)
{
    int a0 = (int)std::floor(x);
    if (a0 > maxDen) { num = a0; den = 1; return; }
    int n0 = 1, d0 = 0, n1 = a0, d1 = 1;
    double frac = x - (double)a0;
    while (frac > 1e-12 && d1 <= maxDen) {
        double inv = 1.0 / frac;
        int ai = (int)std::floor(inv);
        int n2 = n0 + ai * n1, d2 = d0 + ai * d1;
        if (d2 > maxDen) break;
        n0 = n1; d0 = d1; n1 = n2; d1 = d2;
        frac = inv - (double)ai;
    }
    num = n1; den = d1;
}

static const juce::StringArray kSlotParamSuffixes{ "Mute", "Solo", "Rate", "Count", "Gain", "Pan", "Decay", "MidiChannel" };

static juce::String slotParamId(int slotIndex, const juce::String& suffix)
{
    return "slot" + juce::String(slotIndex + 1) + "_" + suffix;
}

static int getSlotCountValue(APVTS& apvts, int slotIndex)
{
    const juce::String countParamId = slotParamId(slotIndex, "Count");

    if (auto* raw = apvts.getRawParameterValue(countParamId))
        return juce::jlimit(1, 64, juce::roundToInt(raw->load()));

    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(countParamId)))
        return juce::jlimit(1, 64, param->get());

    return 4;
}

static constexpr uint64_t kDefaultCountMask = std::numeric_limits<uint64_t>::max();

static uint64_t parseCountMaskVar(const juce::var& value)
{
    if (value.isVoid())
        return kDefaultCountMask;

    if (value.isString())
    {
        auto text = value.toString().trim();
        if (text.isEmpty())
            return kDefaultCountMask;

        if (text.startsWithIgnoreCase("0x"))
            text = text.substring(2);

        const juce::int64 parsed = text.getHexValue64();
        return (uint64_t)(juce::uint64)parsed;
    }

    if (value.isDouble() || value.isInt())
    {
        const double numeric = (double)value;
        if (!std::isfinite(numeric) || numeric < 0.0)
            return kDefaultCountMask;

        if (numeric >= (double)std::numeric_limits<uint64_t>::max())
            return std::numeric_limits<uint64_t>::max();

        return (uint64_t)numeric;
    }

    return kDefaultCountMask;
}

static juce::String serialiseCountMaskValue(uint64_t mask)
{
    auto text = juce::String::toHexString((juce::uint64)mask).toUpperCase();
    return text.paddedLeft('0', 16);
}
}

// 
// SlotVoice implementation
void SlotMachineAudioProcessor::SlotVoice::prepare(double sr)
{
    sampleRate = sr;
    resetPhase(true);
    playIndex = -1;
    playLength = 0;
    env = 0.0f; envAlpha = 1.0f; envSamplesElapsed = 0; envMaxSamples = 0;
    tailSample.setSize(0, 0);
    tailIndex = -1;
    tailLength = 0;
    tailEnv = 0.0f; tailEnvAlpha = 1.0f; tailEnvSamplesElapsed = 0; tailEnvMaxSamples = 0;
    tailPanL = panL;
    tailPanR = panR;
    tailActive = false;
}

void SlotMachineAudioProcessor::SlotVoice::resetPhase(bool hard)
{
    if (hard)
    {
        phase = 0.0;
        framesUntilHit = 0.0;
    }
}

void SlotMachineAudioProcessor::SlotVoice::setPan(float pan)
{
    const float a = juce::jlimit(-1.0f, 1.0f, pan);
    const float theta = juce::jmap(a, -1.0f, 1.0f, 0.0f, juce::MathConstants<float>::halfPi);
    panL = std::cos(theta);
    panR = std::sin(theta);
}

void SlotMachineAudioProcessor::SlotVoice::loadFile(const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    loadReader(std::unique_ptr<juce::AudioFormatReader>(fm.createReaderFor(f)), f.getFullPathName());
}

void SlotMachineAudioProcessor::SlotVoice::loadReader(std::unique_ptr<juce::AudioFormatReader> reader, const juce::String& sourcePath)
{
    active = false;
    sample.setSize(0, 0);
    filePath = {};

    if (reader == nullptr)
        return;

    const int numCh = (int)juce::jlimit(1, 2, (int)reader->numChannels);
    const int safeLen = (int)juce::jlimit<int>(1, (int)reader->lengthInSamples,
        (int)(8 * 60 * reader->sampleRate)); // clamp at 8 minutes
    juce::AudioBuffer<float> tmp(numCh, safeLen);
    reader->read(&tmp, 0, safeLen, 0, true, true);

    const double sourceRate = reader->sampleRate;
    const double targetRate = (sampleRate > 0.0) ? sampleRate : sourceRate;

    if (sourceRate > 0.0 && targetRate > 0.0
        && std::abs(targetRate - sourceRate) > 1.0e-6)
    {
        const double speedRatio = sourceRate / targetRate;
        const int resampledLength = juce::jmax(1, (int)std::ceil((double)tmp.getNumSamples() * (targetRate / sourceRate)));
        juce::AudioBuffer<float> resampled(numCh, resampledLength);

        for (int ch = 0; ch < numCh; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.reset();
            interpolator.process(speedRatio,
                tmp.getReadPointer(ch),
                resampled.getWritePointer(ch),
                resampledLength);
        }

        tmp = std::move(resampled);
    }

    if (numCh == 1)
    {
        sample.setSize(2, tmp.getNumSamples());
        sample.clear();
        sample.copyFrom(0, 0, tmp, 0, 0, tmp.getNumSamples());
        sample.copyFrom(1, 0, tmp, 0, 0, tmp.getNumSamples());
    }
    else
    {
        sample.makeCopyOf(tmp);
    }

    active = (sample.getNumSamples() > 0);
    filePath = sourcePath;

    playIndex = -1;
    playLength = 0;
    env = 0.0f; envSamplesElapsed = 0; envMaxSamples = 0;
}

void SlotMachineAudioProcessor::SlotVoice::trigger()
{
    if (!hasSample())
        return;

    playIndex = 0;
    playLength = sample.getNumSamples();
    ++hitCounter;
    env = 1.0f; envSamplesElapsed = 0;
}

void SlotMachineAudioProcessor::SlotVoice::mixInto(juce::AudioBuffer<float>& io, int numSamples, float gain)
{
    auto mixBuffer = [&io](const juce::AudioBuffer<float>& src, int& index, int length,
        float& envLevel, float& envAlphaRef, int& envSamples, int envSamplesMax,
        float panLeft, float panRight, int numSamplesToProcess, float gainScale)
    {
        if (index < 0 || length <= 0)
            return 0;

        const int remain = length - index;
        const int n = juce::jmin(numSamplesToProcess, remain);

        auto* dstL = io.getWritePointer(0);
        auto* dstR = io.getNumChannels() > 1 ? io.getWritePointer(1) : nullptr;

        const float gL = gainScale * panLeft;
        const float gR = gainScale * panRight;

        const float* srcL = src.getNumSamples() > 0 ? src.getReadPointer(0, index) : nullptr;
        const float* srcR = (src.getNumChannels() > 1 && src.getNumSamples() > 0)
            ? src.getReadPointer(1, index)
            : nullptr;

        if (srcL == nullptr)
            return 0;

        if (dstR != nullptr && srcR != nullptr)
        {
            for (int i = 0; i < n; ++i)
            {
                const float envValue = envLevel;
                dstL[i] += srcL[i] * gL * envValue;
                dstR[i] += srcR[i] * gR * envValue;
                envLevel *= envAlphaRef;
                ++envSamples;
            }
        }
        else if (dstR != nullptr)
        {
            for (int i = 0; i < n; ++i)
            {
                const float s = srcL[i];
                const float envValue = envLevel;
                dstL[i] += s * gL * envValue;
                dstR[i] += s * gR * envValue;
                envLevel *= envAlphaRef;
                ++envSamples;
            }
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                dstL[i] += srcL[i] * gainScale * envLevel;
                envLevel *= envAlphaRef;
                ++envSamples;
            }
        }

        index += n;
        if (envSamplesMax > 0 && envSamples >= envSamplesMax && envLevel < 1.0e-4f)
            index = -1;
        else if (index >= length)
            index = -1;

        return n;
    };

    if (tailActive)
    {
        const int mixed = mixBuffer(tailSample, tailIndex, tailLength,
            tailEnv, tailEnvAlpha, tailEnvSamplesElapsed, tailEnvMaxSamples,
            tailPanL, tailPanR, numSamples, gain);
        if (tailIndex < 0 || mixed <= 0)
        {
            tailSample.setSize(0, 0);
            tailIndex = -1;
            tailLength = 0;
            tailEnv = 0.0f; tailEnvAlpha = 1.0f; tailEnvSamplesElapsed = 0; tailEnvMaxSamples = 0;
            tailPanL = panL; tailPanR = panR;
            tailActive = false;
        }
    }

    mixBuffer(sample, playIndex, playLength,
        env, envAlpha, envSamplesElapsed, envMaxSamples,
        panL, panR, numSamples, gain);
}

void SlotMachineAudioProcessor::SlotVoice::clear(bool allowTail) noexcept
{
    if (allowTail && playIndex >= 0 && playLength > playIndex && sample.getNumSamples() > 0)
    {
        tailSample = std::move(sample);
        tailIndex = playIndex;
        tailLength = playLength;
        tailEnv = env;
        tailEnvAlpha = envAlpha;
        tailEnvSamplesElapsed = envSamplesElapsed;
        tailEnvMaxSamples = envMaxSamples;
        tailPanL = panL;
        tailPanR = panR;
        tailActive = true;
    }
    else if (!allowTail)
    {
        tailSample.setSize(0, 0);
        tailIndex = -1;
        tailLength = 0;
        tailEnv = 0.0f; tailEnvAlpha = 1.0f; tailEnvSamplesElapsed = 0; tailEnvMaxSamples = 0;
        tailPanL = panL; tailPanR = panR;
        tailActive = false;
    }
    else if (!tailActive)
    {
        // allowTail was requested but nothing is currently ringing, ensure clean state
        tailSample.setSize(0, 0);
        tailIndex = -1;
        tailLength = 0;
        tailEnv = 0.0f; tailEnvAlpha = 1.0f; tailEnvSamplesElapsed = 0; tailEnvMaxSamples = 0;
        tailPanL = panL; tailPanR = panR;
        tailActive = false;
    }

    sample.setSize(0, 0);
    active = false;
    filePath = {};
    playIndex = -1;
    playLength = 0;
    phase = 0.0;
    framesUntilHit = 0.0;
    env = 0.0f; envAlpha = 1.0f; envSamplesElapsed = 0; envMaxSamples = 0;
    if (!tailActive)
        tailEnv = 0.0f;
}


//==============================================================================
// Processor
SlotMachineAudioProcessor::SlotMachineAudioProcessor()
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
{
    apvts.state.setProperty(kStateVersionProperty, kCurrentStateVersion, nullptr);
    if (!apvts.state.hasProperty(kAutoInitialiseProperty))
        apvts.state.setProperty(kAutoInitialiseProperty, true, nullptr);
    initialiseOnFirstEditor = static_cast<bool>(apvts.state.getProperty(kAutoInitialiseProperty, true));

    refreshSlotCountMasksFromState();
}

SlotMachineAudioProcessor::~SlotMachineAudioProcessor() {}

//==============================================================================
// Parameters (master, slots, and Options)
APVTS::ParameterLayout SlotMachineAudioProcessor::createParameterLayout()
{
    APVTS::ParameterLayout layout;

    // Master
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "masterBPM", "Master BPM",
        juce::NormalisableRange<float>(10.0f, 1000.0f, 0.01f, 0.33f), 120.0f));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "masterRun", "Master Run", false));

    // Per-slot
    juce::StringArray midiChannelChoices;
    for (int ch = 1; ch <= 16; ++ch)
        midiChannelChoices.add("Ch " + juce::String(ch));

    for (int i = 1; i <= kNumSlots; ++i)
    {
        const auto base = juce::String("slot") + juce::String(i) + "_";

        layout.add(std::make_unique<juce::AudioParameterBool>(base + "Mute", "Slot " + juce::String(i) + " Mute", false));
        layout.add(std::make_unique<juce::AudioParameterBool>(base + "Solo", "Slot " + juce::String(i) + " Solo", false));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            base + "Rate", "Slot " + juce::String(i) + " Rate",
            juce::NormalisableRange<float>(0.0625f, 4.00f, 0.0001f, 0.5f), 1.0f));

        layout.add(std::make_unique<juce::AudioParameterInt>(
            base + "Count", "Slot " + juce::String(i) + " Beats/Cycle",
            1, 64, 4));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            base + "Gain", "Slot " + juce::String(i) + " Gain",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f), 80.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            base + "Pan", "Slot " + juce::String(i) + " Pan",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.0001f, 1.0f), 0.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            base + "Decay", "Slot " + juce::String(i) + " Decay (ms)",
            juce::NormalisableRange<float>(kDecayUiMin, kDecayUiMax, kDecayUiStep, kDecayUiSkew), kDecayUiMax));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            base + "MidiChannel", "Slot " + juce::String(i) + " MIDI Channel",
            midiChannelChoices, juce::jlimit(0, midiChannelChoices.size() - 1, i - 1)));
    }

    // ===== Options (persisted) =====
    layout.add(std::make_unique<juce::AudioParameterBool>(
        "optShowMasterBar", "Show Master Progress Bar", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        "optShowSlotBars", "Show Slot Progress Bars", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        "optShowVisualizer", "Show Visualizer", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        "optVisualizerEdgeWalk", "Visualizer Edge Walk", true));

    layout.add(std::make_unique<juce::AudioParameterInt>(
        "optSampleRate", "Export Sample Rate (Hz)", 44100, 48000, 48000));

    auto mkRGB = [](uint8_t r, uint8_t g, uint8_t b) -> int { return (int)((r << 16) | (g << 8) | b); };

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "optSlotScale", "Slot Row Scale",
        juce::NormalisableRange<float>(0.75f, 1.0f, 0.05f), 0.8f));

    // Glow (selected frame)
    layout.add(std::make_unique<juce::AudioParameterInt>(
        "optGlowColor", "Selected Glow Color (RGB)", 0x000000, 0xFFFFFF, mkRGB(0x69, 0x94, 0xFC)));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "optGlowAlpha", "Selected Glow Alpha", juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.431f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "optGlowWidth", "Selected Glow Width", juce::NormalisableRange<float>(0.5f, 24.0f, 0.01f), 1.34f));

    // Pulse
    layout.add(std::make_unique<juce::AudioParameterInt>(
        "optPulseColor", "Pulse Color (RGB)", 0x000000, 0xFFFFFF, mkRGB(0xD3, 0xCF, 0xE4)));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "optPulseAlpha", "Pulse Alpha", juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "optPulseWidth", "Pulse Width (px)", juce::NormalisableRange<float>(0.5f, 36.0f, 0.01f), 4.0f));

    layout.add(std::make_unique<juce::AudioParameterInt>(
        "optTimingMode", "Timing Mode", 0, 1, 0));

    return layout;
}

//==============================================================================
// Prepare / Release
void SlotMachineAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    masterBeatsAccum = 0.0;

    for (auto& s : slots)
        s.prepare(sampleRate);

    previewVoice.prepare(sampleRate);

    resetAllPhases(true);
}

void SlotMachineAudioProcessor::releaseResources() {}

//==============================================================================
// Processing (MASTER-LOCKED PHASE/HITS)
void SlotMachineAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int  numSamples = buffer.getNumSamples();
    const int  totalOut = getTotalNumOutputChannels();
    const int  totalIn = getTotalNumInputChannels();

    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear(ch, 0, numSamples);

    const bool run = apvts.getRawParameterValue("masterRun")->load();
    const float masterBPM = *apvts.getRawParameterValue("masterBPM");
    const double spb = (masterBPM > 0.0f ? 60.0 / (double)masterBPM : 0.0); // seconds per beat

    // Always emit both audio and MIDI
    const bool wantAudio = true;
    const bool wantMidi = true;

    // Solo mask
    bool anySolo = false;
    bool soloMask[kNumSlots] = {};
    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool solo = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Solo")->load();
        soloMask[i] = solo;
        anySolo = anySolo || solo;
    }

    // Advance master beats accumulator once per block
    const double dtSec = (double)numSamples / currentSampleRate;
    const double prevBeats = masterBeatsAccum;
    if (run && spb > 0.0)
        masterBeatsAccum += dtSec / spb;
    const double currBeats = masterBeatsAccum;
    const int timingMode = (int)std::round(apvts.getRawParameterValue("optTimingMode")->load());
    const double countModeCycleBeats = (double)kCountModeBaseBeats;

    // --- Compute current poly-cycle (in beats), matching Export MIDI logic ---
    int cycleLengthNumerator = 1;
    int cycleLengthDenominator = 1;
    bool hasCycleLength = false;
    const int maxDen = 32;

    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool mute = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Mute")->load();
        if (mute) continue;
        if (anySolo && !soloMask[i]) continue;
        if (!slots[i].hasSample()) continue;

        if (timingMode == 0)
        {
            const float rateF = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Rate");
            const double rate = juce::jmax(0.0001f, rateF);

            int num = 0, den = 1;
            approximateRational(rate, maxDen, num, den);
            int g = igcd(num, den);
            num /= g;
            den /= g;
            accumulateCycleLength(den, num, cycleLengthNumerator, cycleLengthDenominator, hasCycleLength);
        }
        // Beats/Cycle mode does not alter the master cycle length
    }

    double cycleBeats = 1.0;
    if (timingMode == 0)
    {
        if (!hasCycleLength)
        {
            cycleLengthNumerator = 1;
            cycleLengthDenominator = 1;
        }

        cycleBeats = juce::jlimit(1.0e-6, 512.0,
            (double)cycleLengthNumerator / (double)cycleLengthDenominator);
    }
    else
    {
        cycleBeats = juce::jlimit(1.0e-6, 512.0, countModeCycleBeats);
    }

    // Cache for editor
    currentCycleBeats = cycleBeats;
    if (currentCycleBeats > 0.0)
        currentCyclePhase01 = std::fmod(masterBeatsAccum, currentCycleBeats) / currentCycleBeats;
    else
        currentCyclePhase01 = 0.0;

    // Per-slot timing/render
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto& s = slots[i];

        const bool mute = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Mute")->load();
        const bool solo = soloMask[i];

        const float rate = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Rate");
        int count = 4;
        if (auto* countParam = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Count"))
            count = juce::jlimit(1, 64, (int)std::round(countParam->load()));
        const float gainPercent = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Gain");
        const float gain = gainPercent * 0.01f;
        const float pan = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Pan");
        const float decayUi = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Decay");
        const float decayMs = decayUiToMilliseconds(decayUi);
        const auto* midiChoiceRaw = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_MidiChannel");
        int midiChoiceIndex = i;
        if (midiChoiceRaw != nullptr)
            midiChoiceIndex = juce::jlimit(0, 15, (int)std::round(midiChoiceRaw->load()));

        const int midiChannel = juce::jlimit(1, 16, midiChoiceIndex + 1);
       

        s.setPan(pan);
        s.setDecayMs(decayMs);

        // Always keep visual phase tied to master beat phase (even if muted or idle)
        const double rateD = (double)rate;
        if (spb > 0.0)
        {
            if (timingMode == 0 && rateD > 0.0)
            {
                s.phase = std::fmod(currBeats * rateD, 1.0);
            }
            else if (timingMode == 1)
            {
                // --- BeatsPerCycle mode ---
                const double stepBeats = (count > 0 ? countModeCycleBeats / (double)count : 0.0);
                if (stepBeats > 0.0)
                    s.phase = std::fmod(currBeats, stepBeats) / stepBeats;
            }
        }
        else
        {
            // keep previous phase
        }

        // --- manual click triggers (editor requests) ---
        const int manualHits = pendingManualTriggers[(size_t)i].exchange(0, std::memory_order_relaxed);
        if (manualHits > 0)
        {
            if (s.hasSample() && !mute && (!anySolo || solo))
            {
                s.trigger();

                if (wantMidi) {
                    const int noteNumber = 60; // Middle C for all slots
                    const int velocity = juce::jlimit(1, 127, (int)std::round(gain * 127.0f));
                    const int onPos = 0;
                    const int offPos = juce::jmin(numSamples - 1, (int)std::round(0.010 * currentSampleRate));
                    midi.addEvent(juce::MidiMessage::noteOn(midiChannel, noteNumber, (juce::uint8)velocity), onPos);
                    midi.addEvent(juce::MidiMessage::noteOff(midiChannel, noteNumber), offPos);
                }

              
            }
        }

        // Render any currently ringing sample (works even when transport is stopped)
        if (wantAudio && !mute && (!anySolo || solo))
            s.mixInto(buffer, numSamples, gain);

        // No processing if no sample or transport stopped (but visuals still update)
        if (!s.hasSample() || !run || spb <= 0.0)
        {
            continue;
        }

        if (timingMode == 0)
        {
            if (rateD <= 0.0)
                continue;

            // Render any currently ringing sample (only if Audio/Both and not muted/soloed out)
            //if (wantAudio && !mute && (!anySolo || solo))
            //    s.mixInto(buffer, numSamples, gain);

            // Compute how many slot-beats occur within this block
            const double slotBeatsStart = prevBeats * rateD;
            const double slotBeatsEnd = currBeats * rateD;
            const double epsilon = 1e-9;
            const int firstHitCount = (int)std::ceil(slotBeatsStart - epsilon);
            const int endHitExclusive = (int)std::ceil(slotBeatsEnd - epsilon);
            const int hitsThisBlock = juce::jmax(0, endHitExclusive - firstHitCount);

            if (hitsThisBlock > 0)
            {
                for (int h = 0; h < hitsThisBlock; ++h)
                {
                    const double targetCount = (double)(firstHitCount + h);

                    // position inside this block 0..1 for this hit
                    const double denom = (slotBeatsEnd - slotBeatsStart);
                    double fracBlock = 0.0;
                    if (std::abs(denom) > 1e-12)
                        fracBlock = (targetCount - slotBeatsStart) / denom;
                    fracBlock = juce::jlimit(0.0, 1.0, fracBlock);

                    const int hitOffset = juce::jlimit(0, numSamples - 1,
                        (int)std::floor(fracBlock * (double)numSamples + 0.5));

                    // Fire and mix from hit point to block end
                    s.trigger();

                    // MIDI: emit note at exact in-block position
                    if (wantMidi && !mute && (!anySolo || solo))
                    {
                        const int noteNumber = 60; // Middle C for all slots
                        const int velocity = juce::jlimit(1, 127, (int)std::round(gain * 127.0f));

                        const int onPos = hitOffset;
                        const int offPos = juce::jmin(numSamples - 1, hitOffset + (int)std::round(0.010 * currentSampleRate)); // ~10ms

                        midi.addEvent(juce::MidiMessage::noteOn(midiChannel, noteNumber, (juce::uint8)velocity), onPos);
                        midi.addEvent(juce::MidiMessage::noteOff(midiChannel, noteNumber), offPos);
                    }

                    // Audio: mix hit tail from the hit point forward (Audio/Both only)
                    if (wantAudio && !mute && (!anySolo || solo))
                    {
                        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(),
                            buffer.getNumChannels(),
                            hitOffset,
                            numSamples - hitOffset);
                        s.mixInto(view, view.getNumSamples(), gain);
                    }
                }
            }
        }
        else
        {
            // --- BeatsPerCycle mode ---
            const double stepBeats = (count > 0 ? countModeCycleBeats / (double)count : 0.0);
            const double denomBeats = currBeats - prevBeats;
            if (stepBeats <= 0.0 || denomBeats <= 0.0)
                continue;

            const uint64_t activeMask = getSlotCountMask(i) & maskForBeats(count);
            if (activeMask == 0)
                continue;

            const int firstIndex = (int)std::ceil(prevBeats / stepBeats);
            for (int n = firstIndex;; ++n)
            {
                const double hitBeat = (double)n * stepBeats;
                if (hitBeat >= currBeats)
                    break;

                const int beatIndex = (count > 0) ? (n % count) : 0;
                if (((activeMask >> beatIndex) & 1ull) == 0)
                    continue;

                double fracBlock = (hitBeat - prevBeats) / denomBeats;
                fracBlock = juce::jlimit(0.0, 1.0, fracBlock);

                const int hitOffset = juce::jlimit(0, numSamples - 1,
                    (int)std::floor(fracBlock * (double)numSamples + 0.5));

                s.trigger();

                if (wantMidi && !mute && (!anySolo || solo))
                {
                    const int noteNumber = 60; // Middle C for all slots
                    const int velocity = juce::jlimit(1, 127, (int)std::round(gain * 127.0f));

                    const int onPos = hitOffset;
                    const int offPos = juce::jmin(numSamples - 1, hitOffset + (int)std::round(0.010 * currentSampleRate));

                    midi.addEvent(juce::MidiMessage::noteOn(midiChannel, noteNumber, (juce::uint8)velocity), onPos);
                    midi.addEvent(juce::MidiMessage::noteOff(midiChannel, noteNumber), offPos);
                }

                if (wantAudio && !mute && (!anySolo || solo))
                {
                    juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(),
                        buffer.getNumChannels(),
                        hitOffset,
                        numSamples - hitOffset);
                    s.mixInto(view, view.getNumSamples(), gain);
                }
            }
        }
    }

    if (wantAudio)
        previewVoice.mixInto(buffer, numSamples, 1.0f);
}

//==============================================================================
// Editor
juce::AudioProcessorEditor* SlotMachineAudioProcessor::createEditor()
{
    return new SlotMachineAudioProcessorEditor(*this, apvts);
}

void SlotMachineAudioProcessor::initialiseStateForFirstEditor()
{
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            ranged->setValueNotifyingHost(ranged->getDefaultValue());

    clearAllSlots();
    resetAllPhases(false);

    for (int slotIndex = 0; slotIndex < kNumSlots; ++slotIndex)
    {
        const int countValue = getSlotCountValue(apvts, slotIndex);
        const uint64_t fullMask = maskForBeats(countValue);
        setSlotCountMask(slotIndex, fullMask);
    }

    apvts.state.setProperty(kAutoInitialiseProperty, false, nullptr);
}

bool SlotMachineAudioProcessor::consumeInitialiseOnFirstEditor()
{
    const bool shouldInitialise = initialiseOnFirstEditor;
    initialiseOnFirstEditor = false;
    return shouldInitialise;
}

//==============================================================================
// State
void SlotMachineAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Mirror file paths into state
    for (int i = 0; i < kNumSlots; ++i)
    {
        juce::Identifier prop("slot" + juce::String(i + 1) + "_File");
        if (slots[i].getFilePath().isNotEmpty())
            apvts.state.setProperty(prop, slots[i].getFilePath(), nullptr);
        else
            apvts.state.removeProperty(prop, nullptr);
    }

    apvts.state.setProperty(kStateVersionProperty, kCurrentStateVersion, nullptr);

    auto patterns = getPatternsTree();
    const int patternCount = patterns.getNumChildren();
    if (patternCount > 0)
    {
        const int clampedIndex = juce::jlimit(0, patternCount - 1, getCurrentPatternIndex());
        patterns.setProperty(kCurrentPatternIndexProperty, clampedIndex, nullptr);
        storeCurrentStateInPattern(patterns.getChild(clampedIndex));
    }

    auto stateCopy = copyStateWithVersion();
    if (auto xml = stateCopy.createXml())
        copyXmlToBinary(*xml, destData);
}

void SlotMachineAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
    {
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
        upgradeLegacySlotParameters();
        refreshSlotCountMasksFromState();

        // Each new session should start from a blank state regardless of what was stored in the
        // host project.  Force the first editor to reinitialise the processor to its defaults and
        // scrub any persisted slot/tab data from the restored ValueTree.
        apvts.state.setProperty(kAutoInitialiseProperty, true, nullptr);
        initialiseOnFirstEditor = true;

        for (int i = 0; i < kNumSlots; ++i)
        {
            clearSlot(i);

            const auto base = juce::String("slot") + juce::String(i + 1) + "_";
            for (auto& suffix : kSlotParamSuffixes)
                if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(base + suffix)))
                    param->setValueNotifyingHost(param->getDefaultValue());
        }

        if (auto patterns = apvts.state.getChildWithName(kPatternsNodeId); patterns.isValid())
            apvts.state.removeChild(patterns, nullptr);
    }
}

//==============================================================================
// Helpers for editor
void SlotMachineAudioProcessor::resetAllPhases(bool immediate)
{
    for (auto& s : slots)
        s.resetPhase(immediate);
    if (immediate)
    {
        masterBeatsAccum = 0.0;
        currentCyclePhase01 = 0.0;
    }
}

bool SlotMachineAudioProcessor::slotHasSample(int index) const
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    return slots[(size_t)index].hasSample();
}

juce::String SlotMachineAudioProcessor::getSlotFilePath(int index) const
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    return slots[(size_t)index].getFilePath();
}

void SlotMachineAudioProcessor::setSlotFilePath(int index, const juce::String& path)
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    slots[(size_t)index].setFilePath(path);
    apvts.state.setProperty("slot" + juce::String(index + 1) + "_File", path, nullptr);
}

void SlotMachineAudioProcessor::refreshSlotCountMasksFromState()
{
    for (int i = 0; i < kNumSlots; ++i)
    {
        const juce::String propertyId = slotParamId(i, "CountMask");
        const juce::var storedValue = apvts.state.getProperty(propertyId);
        uint64_t mask = parseCountMaskVar(storedValue);

        const int countValue = getSlotCountValue(apvts, i);
        if (countValue > 0)
            mask &= maskForBeats(countValue);
        else
            mask = 0ull;

        countBeatMasks[(size_t)i].store(mask, std::memory_order_relaxed);

        const juce::String serialised = serialiseCountMaskValue(mask);
        if (storedValue.toString() != serialised)
            apvts.state.setProperty(propertyId, serialised, nullptr);
    }
}

bool SlotMachineAudioProcessor::loadSampleForSlot(int index, const juce::File& f, bool allowTail)
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    slots[(size_t)index].clear(allowTail);

    slots[(size_t)index].loadFile(f);

    if (slots[(size_t)index].hasSample())
    {
        apvts.state.setProperty("slot" + juce::String(index + 1) + "_File", f.getFullPathName(), nullptr);
        return true;
    }

    apvts.state.removeProperty("slot" + juce::String(index + 1) + "_File", nullptr);
    return false;
}

bool SlotMachineAudioProcessor::loadEmbeddedSampleForSlot(int index, const juce::String& originalFilename, bool allowTail)
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));

    const auto* sampleInfo = EmbeddedSamples::findByOriginalFilename(originalFilename);
    if (sampleInfo == nullptr)
        return false;

    slots[(size_t)index].clear(allowTail);

    auto reader = createReaderForEmbeddedSample(*sampleInfo);
    if (reader == nullptr)
    {
        apvts.state.removeProperty("slot" + juce::String(index + 1) + "_File", nullptr);
        return false;
    }

    const juce::String storedPath = makeEmbeddedPath(*sampleInfo);
    slots[(size_t)index].loadReader(std::move(reader), storedPath);

    if (slots[(size_t)index].hasSample())
    {
        apvts.state.setProperty("slot" + juce::String(index + 1) + "_File", storedPath, nullptr);
        return true;
    }

    apvts.state.removeProperty("slot" + juce::String(index + 1) + "_File", nullptr);
    return false;
}

void SlotMachineAudioProcessor::previewEmbeddedSample(const juce::String& originalFilename)
{
    const auto* sampleInfo = EmbeddedSamples::findByOriginalFilename(originalFilename);
    if (sampleInfo == nullptr)
        return;

    auto reader = createReaderForEmbeddedSample(*sampleInfo);
    if (reader == nullptr)
        return;

    previewVoice.clear(false);
    previewVoice.loadReader(std::move(reader), {});
    previewVoice.setPan(0.0f);
    previewVoice.setDecayMs(4000.0f);
    previewVoice.trigger();
}

juce::ValueTree SlotMachineAudioProcessor::copyStateWithVersion()
{
    auto stateCopy = apvts.copyState();
    stateCopy.setProperty(kStateVersionProperty, kCurrentStateVersion, nullptr);
    return stateCopy;
}

void SlotMachineAudioProcessor::upgradeLegacySlotParameters()
{
    const int loadedVersion = (int)apvts.state.getProperty(kStateVersionProperty, 0);
    const bool loadedLegacyVersion = loadedVersion < kCurrentStateVersion;

    bool legacyGainsDetected = false;

    auto deriveCountFromRate = [](float rateValue, int minCount, int maxCount)
    {
        if (!std::isfinite(rateValue))
            rateValue = 1.0f;

        const int candidate = juce::roundToInt(rateValue * 4.0f);
        const int clampedMin = juce::jmax(minCount, 1);
        return juce::jlimit(clampedMin, juce::jmax(clampedMin, maxCount), candidate);
    };

    for (int i = 0; i < kNumSlots; ++i)
    {
        const juce::String gainId = "slot" + juce::String(i + 1) + "_Gain";
        if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(gainId)))
        {
            const auto valueVar = apvts.state.getProperty(gainId);
            float stored = 0.0f;
            if (varToFloat(valueVar, stored))
            {
                const auto& range = param->range;
                if (stored <= 1.0f)
                {
                    legacyGainsDetected = true;
                    const float upgraded = juce::jlimit(range.start, range.end, stored * 100.0f);
                    param->setValueNotifyingHost(range.convertTo0to1(upgraded));
                    apvts.state.setProperty(gainId, upgraded, nullptr);
                }
                else if (loadedLegacyVersion)
                {
                    apvts.state.setProperty(gainId, juce::jlimit(range.start, range.end, stored), nullptr);
                }
            }
        }

        const juce::String decayId = "slot" + juce::String(i + 1) + "_Decay";
        if (auto* decayParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(decayId)))
        {
            const auto valueVar = apvts.state.getProperty(decayId);
            float legacyMs = 0.0f;
            if (varToFloat(valueVar, legacyMs))
            {
                const bool shouldUpgradeDecay = legacyGainsDetected || loadedLegacyVersion || legacyMs > kDecayUiMax;
                if (shouldUpgradeDecay)
                {
                    const float upgraded = legacyDecayMsToUi(legacyMs);
                    decayParam->setValueNotifyingHost(decayParam->range.convertTo0to1(upgraded));
                    apvts.state.setProperty(decayId, upgraded, nullptr);
                }
                else if (loadedLegacyVersion)
                {
                    const auto& range = decayParam->range;
                    apvts.state.setProperty(decayId, juce::jlimit(range.start, range.end, legacyMs), nullptr);
                }
            }
        }

        const juce::String rateId = "slot" + juce::String(i + 1) + "_Rate";
        const juce::String countId = "slot" + juce::String(i + 1) + "_Count";

        if (auto* countParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(countId)))
        {
            const auto& countRange = countParam->getNormalisableRange();
            const int minCount = (int)std::round(countRange.start);
            const int maxCount = (int)std::round(countRange.end);

            const bool hasCountProperty = apvts.state.hasProperty(countId);
            if (!hasCountProperty)
            {
                float rateValue = 1.0f;

                if (auto* rateParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(rateId)))
                    rateValue = rateParam->get();
                else
                {
                    const auto rateVar = apvts.state.getProperty(rateId);
                    varToFloat(rateVar, rateValue);
                }

                const int derivedCount = deriveCountFromRate(rateValue, minCount, maxCount);

                apvts.state.setProperty(countId, derivedCount, nullptr);

                const float normalised = countParam->convertTo0to1((float)derivedCount);
                countParam->beginChangeGesture();
                countParam->setValueNotifyingHost(normalised);
                countParam->endChangeGesture();
            }
        }

        const juce::String maskId = slotParamId(i, "CountMask");
        const uint64_t maskValue = parseCountMaskVar(apvts.state.getProperty(maskId));
        apvts.state.setProperty(maskId, serialiseCountMaskValue(maskValue), nullptr);
    }

    if (auto patterns = apvts.state.getChildWithName(kPatternsNodeId); patterns.isValid())
    {
        const int numPatterns = patterns.getNumChildren();
        for (int p = 0; p < numPatterns; ++p)
        {
            auto pattern = patterns.getChild(p);
            for (int slot = 0; slot < kNumSlots; ++slot)
            {
                const juce::String rateId = slotParamId(slot, "Rate");
                const juce::String countId = slotParamId(slot, "Count");

                if (pattern.hasProperty(countId))
                    continue;

                float rateValue = 1.0f;
                const auto rateVar = pattern.getProperty(rateId);
                varToFloat(rateVar, rateValue);

                int minCount = 1;
                int maxCount = 64;
                if (auto* countParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(countId)))
                {
                    const auto& countRange = countParam->getNormalisableRange();
                    minCount = (int)std::round(countRange.start);
                    maxCount = (int)std::round(countRange.end);
                }

                const int derivedCount = deriveCountFromRate(rateValue, minCount, maxCount);
                pattern.setProperty(countId, derivedCount, nullptr);

                const juce::String maskId = slotParamId(slot, "CountMask");
                if (!pattern.hasProperty(maskId))
                    pattern.setProperty(maskId, serialiseCountMaskValue(kDefaultCountMask), nullptr);
            }
        }
    }

    apvts.state.setProperty(kStateVersionProperty, kCurrentStateVersion, nullptr);
    apvts.state.setProperty(kAutoInitialiseProperty, false, nullptr);

    refreshSlotCountMasksFromState();
}

void SlotMachineAudioProcessor::clearSlot(int index, bool allowTail)
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    slots[(size_t)index].clear(allowTail);
    apvts.state.removeProperty("slot" + juce::String(index + 1) + "_File", nullptr);
}

void SlotMachineAudioProcessor::clearAllSlots()
{
    for (int i = 0; i < kNumSlots; ++i)
        clearSlot(i);
}

uint32_t SlotMachineAudioProcessor::getSlotHitCounter(int index) const
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    return slots[(size_t)index].hitCounter;
}

uint64_t SlotMachineAudioProcessor::getSlotCountMask(int index) const
{
    if (!juce::isPositiveAndBelow(index, kNumSlots))
        return kDefaultCountMask;

    return countBeatMasks[(size_t)index].load(std::memory_order_relaxed);
}

void SlotMachineAudioProcessor::setSlotCountMask(int index, uint64_t mask)
{
    if (!juce::isPositiveAndBelow(index, kNumSlots))
        return;

    countBeatMasks[(size_t)index].store(mask, std::memory_order_relaxed);

    const juce::String propertyId = slotParamId(index, "CountMask");
    const juce::String serialised = serialiseCountMaskValue(mask);
    if (apvts.state.getProperty(propertyId).toString() != serialised)
        apvts.state.setProperty(propertyId, serialised, nullptr);
}

uint64_t SlotMachineAudioProcessor::maskForBeats(int beats)
{
    if (beats <= 0)
        return 0ull;

    if (beats >= 64)
        return std::numeric_limits<uint64_t>::max();

    return (1ull << beats) - 1ull;
}

double SlotMachineAudioProcessor::getSlotPhase(int index) const
{
    jassert(juce::isPositiveAndBelow(index, kNumSlots));
    return slots[(size_t)index].phase;
}

double SlotMachineAudioProcessor::getMasterPhase() const
{
    // Averages slot phases that have samples; returns 0 if none
    //double sum = 0.0;
    //int count = 0;
    //for (int i = 0; i < kNumSlots; ++i)
    //{
    //    if (slots[i].hasSample())
    //    {
    //        sum += slots[i].phase;
    //        ++count;
    //    }
    //}
    //return count > 0 ? sum / count : 0.0;
    return currentCyclePhase01; // 0..1 over the full polyrhythmic cycle
}

bool SlotMachineAudioProcessor::exportAudioCycles(const juce::File& destination, int cyclesToExport, juce::String& errorMessage)
{
    errorMessage.clear();

    const double engineSampleRate = currentSampleRate;
    if (engineSampleRate <= 0.0)
    {
        errorMessage = "Audio engine is not initialised.";
        return false;
    }

    double targetSampleRate = engineSampleRate;
    if (auto* sampleRateParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("optSampleRate")))
    {
        const int requested = sampleRateParam->get();

        if (requested == 44100 || requested == 48000)
            targetSampleRate = static_cast<double>(requested);
    }

    const double bpm = (double)*apvts.getRawParameterValue("masterBPM");
    if (bpm <= 0.0)
    {
        errorMessage = "Master BPM must be greater than zero.";
        return false;
    }

    std::array<bool, kNumSlots> soloMask{};
    bool anySolo = false;
    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool solo = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Solo")->load();
        soloMask[(size_t)i] = solo;
        anySolo = anySolo || solo;
    }

    const int timingMode = (int)std::round(apvts.getRawParameterValue("optTimingMode")->load());

    struct OfflineSlot
    {
        SlotVoice voice;
        int num = 0;
        int den = 1;
        int count = 0;
        float gain = 1.0f;
        uint64_t mask = kDefaultCountMask;
        std::vector<int> triggers;
    };

    std::vector<OfflineSlot> slotsToRender;
    slotsToRender.reserve(kNumSlots);

    const double secondsPerBeat = 60.0 / bpm;
    const int maxDen = 32;
    int cycleLengthNumerator = 1;
    int cycleLengthDenominator = 1;
    bool hasCycleLength = false;
    const double countModeCycleBeats = (double)kCountModeBaseBeats;
    juce::StringArray missingFiles;

    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool mute = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Mute")->load();
        if (mute)
            continue;

        if (anySolo && !soloMask[(size_t)i])
            continue;

        const juce::String path = getSlotFilePath(i);
        if (path.isEmpty())
            continue;

        juce::File audioFile(path);
        if (!audioFile.existsAsFile())
        {
            missingFiles.add(audioFile.getFullPathName());
            continue;
        }

        SlotVoice voice;
        voice.prepare(engineSampleRate);
        voice.loadFile(audioFile);
        if (!voice.hasSample())
        {
            missingFiles.add(audioFile.getFullPathName());
            continue;
        }

        const float rateParam = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Rate");
        int count = 4;
        if (auto* countParam = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Count"))
            count = juce::jlimit(1, 64, (int)std::round(countParam->load()));
        const float gainPercent = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Gain");
        const float pan = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Pan");
        const float decayUi = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Decay");

        voice.setPan(pan);
        voice.setDecayMs(decayUiToMilliseconds(decayUi));

        OfflineSlot offline;
        offline.voice = std::move(voice);
        offline.gain = juce::jlimit(0.0f, 1.0f, gainPercent * 0.01f);
        offline.mask = getSlotCountMask(i);

        if (timingMode == 0)
        {
            const double rate = juce::jmax(0.0001f, rateParam);
            int num = 0, den = 1;
            approximateRational(rate, maxDen, num, den);
            const int g = igcd(num, den);
            if (g != 0)
            {
                num /= g;
                den /= g;
            }

            if (num <= 0 || den <= 0)
                continue;

            accumulateCycleLength(den, num, cycleLengthNumerator, cycleLengthDenominator, hasCycleLength);

            offline.num = num;
            offline.den = den;
        }
        else
        {
            // --- BeatsPerCycle mode ---
            offline.count = count;
        }

        slotsToRender.push_back(std::move(offline));
    }

    if (!missingFiles.isEmpty())
    {
        errorMessage = "Missing audio files:\n" + missingFiles.joinIntoString("\n");
        return false;
    }

    if (slotsToRender.empty())
    {
        errorMessage = "No active slots to export.";
        return false;
    }

    double cycleBeats = 1.0;
    if (timingMode == 0)
    {
        if (!hasCycleLength)
        {
            cycleLengthNumerator = 1;
            cycleLengthDenominator = 1;
        }

        cycleBeats = juce::jlimit(1.0e-6, 512.0,
            (double)cycleLengthNumerator / (double)cycleLengthDenominator);
    }
    else
    {
        cycleBeats = juce::jlimit(1.0e-6, 512.0, countModeCycleBeats);
    }

    if (cyclesToExport <= 0)
    {
        errorMessage = "Number of cycles must be positive.";
        return false;
    }

    const double samplesPerBeat = secondsPerBeat * engineSampleRate;
    const double totalBeats = cycleBeats * (double)cyclesToExport;
    const double totalSamplesExact = totalBeats * samplesPerBeat;
    const int totalSamplesTarget = juce::jmax(1, (int)std::round(totalSamplesExact));

    int totalSamplesNeeded = totalSamplesTarget;
    bool anyTriggers = false;

    for (auto& slot : slotsToRender)
    {
        if (timingMode == 0)
        {
            const double hitsPerCycleExact = (cycleBeats * (double)slot.num) / (double)slot.den;
            const int hitsPerCycle = juce::jmax(1, (int)std::llround(hitsPerCycleExact));
            if (hitsPerCycle <= 0)
                continue;

            const double beatSpacing = (double)slot.den / (double)slot.num;
            const int sampleLength = slot.voice.sample.getNumSamples();

            slot.triggers.clear();
            slot.triggers.reserve(hitsPerCycle * juce::jmax(1, cyclesToExport));

            for (int cycle = 0; cycle < cyclesToExport; ++cycle)
            {
                const double cycleBeatOffset = (double)cycle * cycleBeats;

                for (int hit = 0; hit < hitsPerCycle; ++hit)
                {
                    const double beatPosition = cycleBeatOffset + beatSpacing * (double)hit;
                    const double timeSeconds = beatPosition * secondsPerBeat;
                    const int triggerSample = juce::roundToIntAccurate(timeSeconds * engineSampleRate);

                    if (triggerSample < 0 || triggerSample >= totalSamplesTarget)
                        continue;

                    slot.triggers.push_back(triggerSample);
                    anyTriggers = true;

                    const int endSample = triggerSample + sampleLength;
                    totalSamplesNeeded = std::max(totalSamplesNeeded, endSample);
                }
            }
        }
        else
        {
            // --- BeatsPerCycle mode ---
            const int hitsPerCycle = juce::jmax(1, slot.count);
            const double stepBeats = (hitsPerCycle > 0 ? countModeCycleBeats / (double)hitsPerCycle : 0.0);
            if (stepBeats <= 0.0)
                continue;

            const uint64_t mask = slot.mask & maskForBeats(slot.count);
            if (mask == 0)
                continue;

            const int sampleLength = slot.voice.sample.getNumSamples();

            slot.triggers.clear();
            slot.triggers.reserve(hitsPerCycle * juce::jmax(1, cyclesToExport));

            for (int cycle = 0; cycle < cyclesToExport; ++cycle)
            {
                const double cycleBeatOffset = (double)cycle * cycleBeats;

                for (int hit = 0; hit < hitsPerCycle; ++hit)
                {
                    if (((mask >> hit) & 1ull) == 0)
                        continue;

                    const double beatPosition = cycleBeatOffset + stepBeats * (double)hit;
                    const double timeSeconds = beatPosition * secondsPerBeat;
                    const int triggerSample = juce::roundToIntAccurate(timeSeconds * engineSampleRate);

                    if (triggerSample < 0 || triggerSample >= totalSamplesTarget)
                        continue;

                    slot.triggers.push_back(triggerSample);
                    anyTriggers = true;

                    const int endSample = triggerSample + sampleLength;
                    totalSamplesNeeded = std::max(totalSamplesNeeded, endSample);
                }
            }
        }
    }

    if (!anyTriggers || totalSamplesNeeded <= 0)
    {
        errorMessage = "Export length is zero.";
        return false;
    }

    const int numChannels = 2;
    juce::AudioBuffer<float> renderBuffer(numChannels, totalSamplesNeeded);
    renderBuffer.clear();

    for (auto& slot : slotsToRender)
    {
        for (int triggerSample : slot.triggers)
        {
            if (triggerSample < 0 || triggerSample >= totalSamplesNeeded)
                continue;

            slot.voice.trigger();

            const int remaining = totalSamplesNeeded - triggerSample;
            if (remaining <= 0)
                continue;

            juce::AudioBuffer<float> view(renderBuffer.getArrayOfWritePointers(),
                renderBuffer.getNumChannels(), triggerSample, remaining);

            slot.voice.mixInto(view, view.getNumSamples(), slot.gain);
        }
    }

    if (totalSamplesNeeded > totalSamplesTarget)
    {
        const int fadeSamples = juce::jlimit(1, totalSamplesTarget, 512);
        const int fadeStart = totalSamplesTarget - fadeSamples;

        for (int channel = 0; channel < renderBuffer.getNumChannels(); ++channel)
            renderBuffer.applyGainRamp(channel, fadeStart, fadeSamples, 1.0f, 0.0f);
    }

    juce::AudioBuffer<float>* bufferToWrite = &renderBuffer;
    int samplesToWrite = totalSamplesTarget;
    juce::AudioBuffer<float> resampledBuffer;

    if (targetSampleRate != engineSampleRate)
    {
        const double resampleRatio = targetSampleRate / engineSampleRate;
        const int outputSamples = juce::jmax(1, juce::roundToIntAccurate((double)totalSamplesTarget * resampleRatio));
        resampledBuffer.setSize(numChannels, outputSamples);

        const double sampleRatio = engineSampleRate / targetSampleRate;
        const int maxSourceIndex = juce::jmax(0, totalSamplesTarget - 1);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const float* src = renderBuffer.getReadPointer(channel);
            float* dst = resampledBuffer.getWritePointer(channel);

            for (int i = 0; i < outputSamples; ++i)
            {
                const double srcIndex = (double)i * sampleRatio;
                int index = (int)srcIndex;
                double frac = srcIndex - (double)index;

                index = juce::jlimit(0, maxSourceIndex, index);
                const int nextIndex = juce::jlimit(0, maxSourceIndex, index + 1);

                const float s0 = src[index];
                const float s1 = src[nextIndex];
                dst[i] = s0 + (s1 - s0) * (float)frac;
            }
        }

        bufferToWrite = &resampledBuffer;
        samplesToWrite = outputSamples;
    }

    if (destination.existsAsFile())
    {
        if (!destination.deleteFile())
        {
            errorMessage = "Couldn't overwrite existing file:\n" + destination.getFullPathName();
            return false;
        }
    }

    std::unique_ptr<juce::FileOutputStream> stream(destination.createOutputStream());
    if (stream == nullptr || !stream->openedOk())
    {
        errorMessage = "Couldn't open file for writing:\n" + destination.getFullPathName();
        return false;
    }

    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), targetSampleRate,
        (unsigned int)bufferToWrite->getNumChannels(), 24, {}, 0));

    if (writer == nullptr)
    {
        errorMessage = "Couldn't create WAV writer.";
        return false;
    }

    stream.release();

    const bool ok = writer->writeFromAudioSampleBuffer(*bufferToWrite, 0, samplesToWrite);
    writer.reset();

    if (!ok)
    {
        errorMessage = "Failed to write audio data.";
        return false;
    }

    return true;
}

//==============================================================================
// Pattern helpers
juce::ValueTree SlotMachineAudioProcessor::getPatternsTree()
{
    auto patterns = apvts.state.getChildWithName(kPatternsNodeId);
    if (!patterns.isValid())
    {
        patterns = juce::ValueTree(kPatternsNodeId);
        apvts.state.addChild(patterns, -1, nullptr);
    }

    if (!patterns.hasProperty(kCurrentPatternIndexProperty))
        patterns.setProperty(kCurrentPatternIndexProperty, 0, nullptr);

    if (patterns.getNumChildren() == 0)
    {
        auto initial = createPatternTreeFromCurrentState("A");
        patterns.addChild(initial, -1, nullptr);
    }

    return patterns;
}

juce::ValueTree SlotMachineAudioProcessor::createDefaultPatternTree(const juce::String& name) const
{
    juce::ValueTree pattern(kPatternNodeType);
    pattern.setProperty(kPatternNameProperty, name, nullptr);

    if (auto* masterParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("masterBPM")))
    {
        const auto* ranged = static_cast<juce::RangedAudioParameter*>(masterParam);
        const float defaultValue = masterParam->convertFrom0to1(ranged->getDefaultValue());
        pattern.setProperty(kPatternMasterBpmProperty, defaultValue, nullptr);
    }

    if (auto* timingParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("optTimingMode")))
    {
        const auto* ranged = static_cast<juce::RangedAudioParameter*>(timingParam);
        const float defaultNormalised = ranged->getDefaultValue();
        const int defaultValue = (int)std::round(timingParam->convertFrom0to1(defaultNormalised));
        pattern.setProperty(kPatternTimingModeProperty, defaultValue, nullptr);
    }

    for (int slot = 0; slot < kNumSlots; ++slot)
    {
        for (auto& suffix : kSlotParamSuffixes)
        {
            const juce::String paramId = slotParamId(slot, suffix);
            if (auto* parameter = apvts.getParameter(paramId))
            {
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
                {
                    if (auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(parameter))
                    {
                        const bool defaultValue = ranged->getDefaultValue() >= 0.5f;
                        pattern.setProperty(paramId, defaultValue, nullptr);
                    }
                    else if (auto* intParam = dynamic_cast<juce::AudioParameterInt*>(parameter))
                    {
                        const float defaultNormalised = ranged->getDefaultValue();
                        const int defaultValue = (int)std::round(intParam->convertFrom0to1(defaultNormalised));
                        pattern.setProperty(paramId, defaultValue, nullptr);
                    }
                    else if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*>(parameter))
                    {
                        const float defaultValue = floatParam->convertFrom0to1(ranged->getDefaultValue());
                        pattern.setProperty(paramId, defaultValue, nullptr);
                    }
                    else if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(parameter))
                    {
                        const int defaultIndex = juce::jlimit(0, choiceParam->choices.size() - 1,
                            (int)std::round(choiceParam->convertFrom0to1(ranged->getDefaultValue())));
                        pattern.setProperty(paramId, defaultIndex, nullptr);
                    }
                }
            }
        }

        const juce::String fileId = slotParamId(slot, "File");
        pattern.setProperty(fileId, juce::String(), nullptr);
    }

    return pattern;
}

juce::ValueTree SlotMachineAudioProcessor::createPatternTreeFromCurrentState(const juce::String& name) const
{
    juce::ValueTree pattern = createDefaultPatternTree(name);
    storeCurrentStateInPattern(pattern);
    return pattern;
}

void SlotMachineAudioProcessor::storeCurrentStateInPattern(juce::ValueTree pattern) const
{
    if (!pattern.isValid())
        return;

    if (auto* masterParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("masterBPM")))
        pattern.setProperty(kPatternMasterBpmProperty, masterParam->get(), nullptr);

    if (auto* timingParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("optTimingMode")))
        pattern.setProperty(kPatternTimingModeProperty, timingParam->get(), nullptr);

    for (int slot = 0; slot < kNumSlots; ++slot)
    {
        for (auto& suffix : kSlotParamSuffixes)
        {
            const juce::String paramId = slotParamId(slot, suffix);

            if (auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(paramId)))
            {
                pattern.setProperty(paramId, boolParam->get(), nullptr);
            }
            else if (auto* intParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(paramId)))
            {
                pattern.setProperty(paramId, intParam->get(), nullptr);
            }
            else if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramId)))
            {
                pattern.setProperty(paramId, floatParam->get(), nullptr);
            }
            else if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramId)))
            {
                pattern.setProperty(paramId, choiceParam->getIndex(), nullptr);
            }
        }

        const juce::String fileId = slotParamId(slot, "File");
        pattern.setProperty(fileId, slots[(size_t)slot].getFilePath(), nullptr);

        const juce::String maskId = slotParamId(slot, "CountMask");
        pattern.setProperty(maskId, apvts.state.getProperty(maskId), nullptr);
    }
}

void SlotMachineAudioProcessor::applyPatternTree(const juce::ValueTree& pattern, juce::Array<int>* failedSlots, bool allowTailRelease)
{
    if (failedSlots)
        failedSlots->clear();

    if (!pattern.isValid())
        return;

    if (auto* masterParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("masterBPM")))
    {
        const auto valueVar = pattern.getProperty(kPatternMasterBpmProperty);
        const float current = masterParam->get();
        float target = valueVar.isVoid() ? current : (float)valueVar;
        target = juce::jlimit(masterParam->range.start, masterParam->range.end, target);

        masterParam->beginChangeGesture();
        masterParam->setValueNotifyingHost(masterParam->convertTo0to1(target));
        masterParam->endChangeGesture();
    }

    if (auto* timingParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("optTimingMode")))
    {
        const auto valueVar = pattern.getProperty(kPatternTimingModeProperty);
        const int current = timingParam->get();
        int target = valueVar.isVoid() ? current : (int)valueVar;
        const auto& range = timingParam->getNormalisableRange();
        const int minValue = (int)std::round(range.start);
        const int maxValue = (int)std::round(range.end);
        target = juce::jlimit(minValue, maxValue, target);

        timingParam->beginChangeGesture();
        timingParam->setValueNotifyingHost(timingParam->convertTo0to1((float)target));
        timingParam->endChangeGesture();
    }

    for (int slot = 0; slot < kNumSlots; ++slot)
    {
        for (auto& suffix : kSlotParamSuffixes)
        {
            const juce::String paramId = slotParamId(slot, suffix);
            const auto valueVar = pattern.getProperty(paramId);

            if (auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(paramId)))
            {
                const bool target = valueVar.isVoid() ? boolParam->get() : (bool)valueVar;
                boolParam->beginChangeGesture();
                *boolParam = target;
                boolParam->endChangeGesture();
            }
            else if (auto* intParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(paramId)))
            {
                const int current = intParam->get();
                int target = valueVar.isVoid() ? current : (int)valueVar;
                const auto range = intParam->getNormalisableRange();
                const int minValue = (int)std::round(range.start);
                const int maxValue = (int)std::round(range.end);
                target = juce::jlimit(minValue, maxValue, target);
                intParam->beginChangeGesture();
                intParam->setValueNotifyingHost(intParam->convertTo0to1((float)target));
                intParam->endChangeGesture();
            }
            else if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramId)))
            {
                const float current = floatParam->get();
                const float target = valueVar.isVoid() ? current : (float)valueVar;
                floatParam->beginChangeGesture();
                floatParam->setValueNotifyingHost(floatParam->convertTo0to1(target));
                floatParam->endChangeGesture();
            }
            else if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramId)))
            {
                const int current = choiceParam->getIndex();
                int target = valueVar.isVoid() ? current : (int)valueVar;
                target = juce::jlimit(0, choiceParam->choices.size() - 1, target);
                choiceParam->beginChangeGesture();
                choiceParam->setValueNotifyingHost(choiceParam->convertTo0to1(target));
                choiceParam->endChangeGesture();
            }
        }

        const juce::String fileId = slotParamId(slot, "File");
        const juce::String path = pattern.getProperty(fileId).toString();

        const juce::String maskId = slotParamId(slot, "CountMask");
        const uint64_t maskValue = parseCountMaskVar(pattern.getProperty(maskId));
        setSlotCountMask(slot, maskValue);

        if (path.isNotEmpty())
        {
            bool loaded = false;
            if (isEmbeddedSamplePath(path))
            {
                const juce::String embeddedName = extractEmbeddedFilename(path);
                loaded = loadEmbeddedSampleForSlot(slot, embeddedName, allowTailRelease);
            }
            else
            {
                const juce::File file(path);
                loaded = loadSampleForSlot(slot, file, allowTailRelease);
            }

            if (!loaded)
            {
                clearSlot(slot, allowTailRelease);
                setSlotFilePath(slot, path);
                if (failedSlots)
                    failedSlots->addIfNotAlreadyThere(slot);
            }
        }
        else
        {
            clearSlot(slot, allowTailRelease);
        }
    }
}

void SlotMachineAudioProcessor::setCurrentPatternIndex(int index)
{
    auto patterns = getPatternsTree();
    patterns.setProperty(kCurrentPatternIndexProperty, juce::jmax(0, index), nullptr);
}

int SlotMachineAudioProcessor::getCurrentPatternIndex() const
{
    auto patterns = apvts.state.getChildWithName(kPatternsNodeId);
    if (!patterns.isValid())
        return 0;

    return (int)patterns.getProperty(kCurrentPatternIndexProperty, 0);
}

//==============================================================================
// Factory
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlotMachineAudioProcessor();
}

void SlotMachineAudioProcessor::SlotVoice::setDecayMs(float ms)
{
    if (ms <= 0.0f || sampleRate <= 0.0)
    { envAlpha = 1.0f; envMaxSamples = 0; return; }
    const double samples = (ms / 1000.0) * sampleRate;
    envMaxSamples = (int) std::round(samples);
    envAlpha = (float) std::pow(0.001, 1.0 / juce::jmax(1.0, samples));
}

void SlotMachineAudioProcessor::requestManualTrigger(int index)
{
    if (index < 0 || index >= kNumSlots)
        return;

    pendingManualTriggers[(size_t)index].fetch_add(1, std::memory_order_relaxed);
}
