#include "CountBeatMaskGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    static uint64_t maskForBeatsInternal(int beats)
    {
        if (beats <= 0)
            return 0ull;

        if (beats >= 64)
            return std::numeric_limits<uint64_t>::max();

        return (1ull << beats) - 1ull;
    }
}

static int normaliseColumns(int columns, int beats)
{
    if (columns > 0)
        return columns;

    const double root = std::ceil(std::sqrt((double)std::max(1, beats)));
    return juce::jlimit(1, std::max(1, beats), (int)root);
}

uint64_t CountBeatMaskGrid::limitMaskToBeats(uint64_t currentMask, int beats)
{
    return currentMask & maskForBeatsInternal(beats);
}

CountBeatMaskGrid::CountBeatMaskGrid(Options opts,
                                     uint64_t initialMask,
                                     std::function<void(uint64_t)> onMaskChanged)
    : options(opts)
    , mask(limitMaskToBeats(initialMask, juce::jlimit(1, 64, options.beats)))
    , maskChanged(std::move(onMaskChanged))
{
    options.beats = juce::jlimit(1, 64, options.beats);
    if (options.columns <= 0)
        options.columns = normaliseColumns(options.columns, options.beats);
    else
        options.columns = juce::jlimit(1, std::max(1, options.beats), options.columns);

    mask = limitMaskToBeats(mask, options.beats);

    buildButtons();

    const int columns = juce::jmax(1, options.columns);
    const int rows = (options.beats + columns - 1) / columns;
    const int width = columns * options.buttonW + juce::jmax(0, columns - 1) * options.gap + options.gap * 2;
    const int height = rows * options.buttonH + juce::jmax(0, rows - 1) * options.gap + options.gap * 2;
    setSize(width, height);
}

void CountBeatMaskGrid::buildButtons()
{
    buttons.clear();

    for (int beat = 0; beat < options.beats; ++beat)
    {
        auto button = std::make_unique<juce::TextButton>(juce::String(beat + 1));
        button->setClickingTogglesState(true);
        const bool selected = ((mask >> beat) & 1ull) != 0;
        button->setToggleState(selected, juce::dontSendNotification);
        button->setColour(juce::TextButton::buttonColourId, juce::Colours::dimgrey.withAlpha(0.85f));
        button->setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        button->setColour(juce::TextButton::textColourOffId, juce::Colours::whitesmoke);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        button->addListener(this);
        addAndMakeVisible(button.get());
        buttons.add(button.release());
    }
}

void CountBeatMaskGrid::resized()
{
    auto area = getLocalBounds().reduced(options.gap);
    const int columns = juce::jmax(1, options.columns);

    for (int index = 0; index < buttons.size(); ++index)
    {
        const int column = index % columns;
        const int row = index / columns;
        const int x = area.getX() + column * (options.buttonW + options.gap);
        const int y = area.getY() + row * (options.buttonH + options.gap);
        if (auto* button = buttons[index])
            button->setBounds(x, y, options.buttonW, options.buttonH);
    }
}

void CountBeatMaskGrid::buttonClicked(juce::Button* button)
{
    auto* textButton = dynamic_cast<juce::TextButton*>(button);
    if (textButton == nullptr)
        return;

    const int index = buttons.indexOf(textButton);
    if (index < 0 || index >= options.beats)
        return;

    const bool selected = textButton->getToggleState();
    const uint64_t bit = (index < 64) ? (1ull << index) : 0ull;
    if (bit == 0)
        return;

    if (selected)
        mask |= bit;
    else
        mask &= ~bit;

    mask = limitMaskToBeats(mask, options.beats);

    if (maskChanged)
        maskChanged(limitMaskToBeats(mask, options.beats));
}
