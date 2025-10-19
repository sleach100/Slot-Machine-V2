#include "BeatsQuickPickGrid.h"

#include <utility>

namespace
{
    constexpr auto kHighlightColour = juce::Colours::lightblue;
}

BeatsQuickPickGrid::BeatsQuickPickGrid(Options opts,
                                       std::function<void(int)> onPick,
                                       int currentValue)
    : options(opts), pickCallback(std::move(onPick)), current(currentValue)
{
    expanded = options.maxBeat > 32;
    buildButtons();

    if (options.showExpandToggle)
    {
        const auto* toggleText = expanded ? "Show 1–32" : "Show 33–64";
        expandToggle = std::make_unique<juce::TextButton>(toggleText);
        expandToggle->onClick = [this]()
        {
            expanded = !expanded;
            expandToggle->setButtonText(expanded ? "Show 1–32" : "Show 33–64");
            rebuildForRange(expanded ? 64 : 32);
        };
        addAndMakeVisible(*expandToggle);
    }
}

void BeatsQuickPickGrid::buildButtons()
{
    for (auto* button : buttons)
        removeChildComponent(button);

    buttons.clear();

    for (int value = options.minBeat; value <= options.maxBeat; ++value)
    {
        auto* button = new juce::TextButton(juce::String(value));
        if (value == current)
            button->setColour(juce::TextButton::buttonColourId, kHighlightColour);

        button->addListener(this);
        addAndMakeVisible(button);
        buttons.add(button);
    }

    resized();
}

void BeatsQuickPickGrid::rebuildForRange(int newMax)
{
    options.maxBeat = newMax;
    expanded = newMax > 32;
    buildButtons();
    repaint();
}

void BeatsQuickPickGrid::resized()
{
    const int gap   = options.gap;
    const int bw    = options.buttonW;
    const int bh    = options.buttonH;
    const int cols  = options.columns;

    int x = gap;
    int y = gap;
    int col = 0;

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* button = buttons[i])
            button->setBounds(x, y, bw, bh);

        x += bw + gap;
        if (++col >= cols)
        {
            col = 0;
            x = gap;
            y += bh + gap;
        }
    }

    if (expandToggle)
    {
        expandToggle->setBounds(gap, y + gap, cols * (bw + gap) - gap, bh);
        y += bh + gap * 2;
    }

    setSize(cols * (bw + gap) + gap, y + gap);
}

void BeatsQuickPickGrid::buttonClicked(juce::Button* b)
{
    const int value = b->getButtonText().getIntValue();
    if (pickCallback)
        pickCallback(value);

    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
        callout->dismiss();
}
