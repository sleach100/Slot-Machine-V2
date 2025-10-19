#include "BeatsQuickPickGrid.h"

#include <utility>

namespace
{
    static constexpr juce::Colour kHighlightColour = juce::Colours::lightblue;
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
        expandToggle = std::make_unique<juce::TextButton>(expanded ? "Show 1–32" : "Show 33–64");
        expandToggle->onClick = [this]()
        {
            expanded = !expanded;
            rebuildForRange(expanded ? 64 : 32);
        };
        addAndMakeVisible(*expandToggle);
    }

    updateSizeForContent();
}

void BeatsQuickPickGrid::buildButtons()
{
    buttons.clear();

    for (int v = options.minBeat; v <= options.maxBeat; ++v)
    {
        auto* btn = new juce::TextButton(juce::String(v));
        btn->setClickingTogglesState(false);
        btn->addListener(this);
        btn->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        btn->setColour(juce::TextButton::buttonOnColourId, kHighlightColour);
        btn->setColour(juce::TextButton::textColourOffId, juce::Colours::white);

        if (v == current)
            btn->setColour(juce::TextButton::buttonColourId, kHighlightColour.withAlpha(0.6f));

        addAndMakeVisible(btn);
        buttons.add(btn);
    }
}

void BeatsQuickPickGrid::rebuildForRange(int newMax)
{
    options.maxBeat = newMax;
    expanded = newMax > 32;

    for (auto* b : buttons)
        removeChildComponent(b);

    buildButtons();
    updateToggleForExpansion();
    updateSizeForContent();
    repaint();
}

void BeatsQuickPickGrid::updateToggleForExpansion()
{
    if (expandToggle)
        expandToggle->setButtonText(expanded ? "Show 1–32" : "Show 33–64");
}

void BeatsQuickPickGrid::updateSizeForContent()
{
    const int totalButtons = options.maxBeat - options.minBeat + 1;
    const int cols = juce::jmax(1, options.columns);
    const int rows = juce::jmax(1, (totalButtons + cols - 1) / cols);
    const int gap = options.gap;
    const int width = cols * (options.buttonW + gap) + gap;

    int height = gap + rows * (options.buttonH + gap);
    if (expandToggle)
        height += options.buttonH + gap * 2;

    setSize(width, height);
}

void BeatsQuickPickGrid::resized()
{
    const int gap = options.gap;
    const int bw  = options.buttonW;
    const int bh  = options.buttonH;
    const int cols = options.columns;

    int x = gap;
    int y = gap;
    int col = 0;

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* button = buttons[i])
        {
            button->setBounds(x, y, bw, bh);
        }

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
}

void BeatsQuickPickGrid::buttonClicked(juce::Button* b)
{
    if (b == expandToggle.get())
        return;

    const int value = b->getButtonText().getIntValue();

    if (pickCallback)
        pickCallback(value);

    if (auto* p = findParentComponentOfClass<juce::CallOutBox>())
        p->dismiss();
}
