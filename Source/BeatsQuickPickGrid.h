#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

class BeatsQuickPickGrid : public juce::Component,
                           private juce::Button::Listener
{
public:
    struct Options {
        int minBeat = 1;
        int maxBeat = 32; // default human range
        int columns = 8;  // 8x4 grid for 1–32
        int buttonW = 36;
        int buttonH = 28;
        int gap = 6;
        bool showExpandToggle = true;
    };

    BeatsQuickPickGrid(Options opts,
                       std::function<void(int)> onPick,
                       int currentValue);
    BeatsQuickPickGrid(Options opts,
                       std::function<void(uint32_t)> onMaskConfirm,
                       uint32_t initialMask,
                       int editableBeatLimit = 32);
    ~BeatsQuickPickGrid() override = default;

    void resized() override;

private:
    void buildButtons();
    void rebuildForRange(int newMax);
    void buttonClicked(juce::Button* b) override;
    void initialiseMaskSelection();
    void updateMaskButtonState(int buttonIndex);
    void commitMask(bool accepted);
    bool isBeatEditable(int beat) const;

    Options options;
    std::function<void(int)> pickCallback;
    std::function<void(uint32_t)> maskCallback;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::TextButton> expandToggle;
    int current = 1;
    bool expanded = false;
    bool maskMode = false;
    uint32_t maskValue = 0;
    int maskEditableLimit = 32;
    std::vector<bool> maskSelected;
    juce::TextButton okButton{ "OK" };
    juce::TextButton cancelButton{ "Cancel" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatsQuickPickGrid)
};
