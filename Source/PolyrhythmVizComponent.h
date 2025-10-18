#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <vector>

#include "PluginProcessor.h"

class PolyrhythmVizComponent : public juce::Component, private juce::Timer
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    PolyrhythmVizComponent(SlotMachineAudioProcessor& processor, APVTS& state);
    ~PolyrhythmVizComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateSlotGeometry(int slotIndex, juce::Point<float> centre, float radius);
    static void approximateRational(double value, int maxDenominator, int& num, int& den);

    SlotMachineAudioProcessor& processor;
    APVTS& apvts;

    struct SlotVisual
    {
        bool active = false;
        int sides = 0;
        double beadPhase = 0.0;
        double beadAngle = 0.0;
        juce::Point<float> centre{};
        float radius = 0.0f;
        juce::Path polygonPath;
        std::vector<juce::Point<float>> vertices;
        float flash = 0.0f;
        int flashVertex = -1;
        uint32_t lastHitCounter = 0;
        juce::Colour colour;
        juce::Point<float> beadPos{};
        bool edgeWalk = true;
    };

    static constexpr int kNumSlots = SlotMachineAudioProcessor::kNumSlots;

    std::array<SlotVisual, kNumSlots> slotVisuals{};
    std::array<int, kNumSlots> activeOrder{};
    int activeCount = 0;

    double masterPhase = 0.0;
    double lastPhase = 0.0;
    float wrapFlash = 0.0f;
};
