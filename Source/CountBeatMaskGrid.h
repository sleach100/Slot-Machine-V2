#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <cstdint>

class CountBeatMaskGrid : public juce::Component,
                          private juce::Button::Listener
{
public:
    struct Options
    {
        int beats = 1;
        int columns = 4;
        int buttonW = 32;
        int buttonH = 28;
        int gap = 6;
    };

    CountBeatMaskGrid(Options options,
                      uint64_t initialMask,
                      std::function<void(uint64_t)> onMaskChanged);
    ~CountBeatMaskGrid() override = default;

    void resized() override;

private:
    void buildButtons();
    void buttonClicked(juce::Button* button) override;

    static uint64_t limitMaskToBeats(uint64_t mask, int beats);

    Options options;
    uint64_t mask = 0;
    std::function<void(uint64_t)> maskChanged;
    juce::OwnedArray<juce::TextButton> buttons;
};
