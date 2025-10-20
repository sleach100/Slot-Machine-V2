#include "BeatsQuickPickGrid.h"

#include <utility>

namespace
{
    const juce::Colour kHighlightColour = juce::Colours::lightblue;
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

BeatsQuickPickGrid::BeatsQuickPickGrid(Options opts,
                                       std::function<void(uint32_t)> onMaskConfirm,
                                       uint32_t initialMask,
                                       int editableBeatLimit)
    : options(opts), maskCallback(std::move(onMaskConfirm)), maskMode(true), maskValue(initialMask)
{
    options.showExpandToggle = false;
    expanded = options.maxBeat > 32;
    maskEditableLimit = juce::jlimit(0, 32, editableBeatLimit);
    okButton.addListener(this);
    cancelButton.addListener(this);
    initialiseMaskSelection();
    buildButtons();
}

void BeatsQuickPickGrid::buildButtons()
{
    for (auto* button : buttons)
        removeChildComponent(button);

    buttons.clear();

    if (maskMode)
        initialiseMaskSelection();

    for (int value = options.minBeat; value <= options.maxBeat; ++value)
    {
        auto* button = new juce::TextButton(juce::String(value));
        if (maskMode)
        {
            const int idx = value - options.minBeat;
            const bool selected = juce::isPositiveAndBelow(idx, (int)maskSelected.size()) ? maskSelected[(size_t)idx] : true;
            if (selected)
                button->setColour(juce::TextButton::buttonColourId, kHighlightColour);

            const bool editable = isBeatEditable(value);
            button->setEnabled(editable);
            if (!editable)
                button->setTooltip("Masking supports up to 32 beats per slot.");
        }
        else if (value == current)
        {
            button->setColour(juce::TextButton::buttonColourId, kHighlightColour);
        }

        button->addListener(this);
        addAndMakeVisible(button);
        buttons.add(button);
    }

    if (maskMode)
    {
        addAndMakeVisible(okButton);
        addAndMakeVisible(cancelButton);
    }
    else if (expandToggle)
    {
        addAndMakeVisible(*expandToggle);
    }

    resized();
}

void BeatsQuickPickGrid::rebuildForRange(int newMax)
{
    if (maskMode)
    {
        options.maxBeat = newMax;
        buildButtons();
        repaint();
        return;
    }

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

    if (maskMode)
    {
        const int totalWidth = cols * (bw + gap) + gap;
        const int rowWidth = totalWidth - 2 * gap;

        int buttonsBottom = gap;
        if (buttons.size() > 0)
        {
            if (col == 0)
                buttonsBottom = y - gap; // y already advanced past final row
            else
                buttonsBottom = y + bh;   // y still at top of final row
        }

        const int controlsTop = buttonsBottom + gap;
        juce::Rectangle<int> rowArea(gap, controlsTop, rowWidth, bh);
        const int buttonGap = juce::jmax(4, gap);
        const int halfWidth = juce::jmax(40, (rowArea.getWidth() - buttonGap) / 2);
        auto okArea = rowArea.removeFromLeft(halfWidth);
        rowArea.removeFromLeft(buttonGap);
        auto cancelArea = rowArea;

        okButton.setBounds(okArea);
        cancelButton.setBounds(cancelArea);

        const int totalHeight = cancelArea.getBottom() + gap;
        setSize(totalWidth, totalHeight + gap);
        return;
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
    if (maskMode)
    {
        if (b == &okButton)
        {
            commitMask(true);
            return;
        }

        if (b == &cancelButton)
        {
            commitMask(false);
            return;
        }

        const int beat = b->getButtonText().getIntValue();
        const int index = beat - options.minBeat;
        if (!juce::isPositiveAndBelow(index, (int)maskSelected.size()))
            return;

        if (!isBeatEditable(beat))
            return;

        maskSelected[(size_t)index] = !maskSelected[(size_t)index];

        const int bit = beat - 1;
        if (maskSelected[(size_t)index])
            maskValue |= (1u << bit);
        else
            maskValue &= ~(1u << bit);

        updateMaskButtonState(index);
        return;
    }

    const int value = b->getButtonText().getIntValue();
    if (pickCallback)
        pickCallback(value);

    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
        callout->dismiss();
}

void BeatsQuickPickGrid::initialiseMaskSelection()
{
    const int total = options.maxBeat - options.minBeat + 1;
    maskSelected.assign((size_t)juce::jmax(0, total), true);

    const int controllableBeats = juce::jmin(maskEditableLimit, options.maxBeat);
    if (controllableBeats >= 32)
        maskValue &= 0xFFFFFFFFu;
    else if (controllableBeats <= 0)
        maskValue = 0u;
    else
        maskValue &= ((1u << controllableBeats) - 1u);

    for (int idx = 0; idx < total; ++idx)
    {
        const int beat = options.minBeat + idx;
        if (isBeatEditable(beat))
        {
            const int bit = beat - 1;
            if (bit >= 0 && bit < 32)
                maskSelected[(size_t)idx] = ((maskValue >> bit) & 1u) != 0u;
            else
                maskSelected[(size_t)idx] = true;
        }
        else
        {
            maskSelected[(size_t)idx] = true;
        }
    }
}

void BeatsQuickPickGrid::updateMaskButtonState(int buttonIndex)
{
    if (!juce::isPositiveAndBelow(buttonIndex, buttons.size()))
        return;

    if (auto* button = buttons[buttonIndex])
    {
        const bool selected = juce::isPositiveAndBelow(buttonIndex, (int)maskSelected.size()) ? maskSelected[(size_t)buttonIndex] : true;
        if (selected)
            button->setColour(juce::TextButton::buttonColourId, kHighlightColour);
        else
            button->removeColour(juce::TextButton::buttonColourId);
    }
}

void BeatsQuickPickGrid::commitMask(bool accepted)
{
    if (!maskMode)
        return;

    if (accepted && maskCallback)
    {
        const int controllableBeats = juce::jmin(maskEditableLimit, options.maxBeat);
        uint32_t allowed = 0xFFFFFFFFu;
        if (controllableBeats >= 32)
            allowed = 0xFFFFFFFFu;
        else if (controllableBeats <= 0)
            allowed = 0u;
        else
            allowed = (uint32_t)((1u << controllableBeats) - 1u);

        maskValue &= allowed;
        maskCallback(maskValue);
    }

    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
        callout->dismiss();
}

bool BeatsQuickPickGrid::isBeatEditable(int beat) const
{
    return beat <= maskEditableLimit;
}
