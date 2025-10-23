#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void setCornerRadius (float r) noexcept { cornerRadius = r; }
    float getCornerRadius () const noexcept { return cornerRadius; }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour& bg, bool isOver, bool isDown) override
    {
        auto bounds = b.getLocalBounds().toFloat();
        auto base = bg;

        if (isDown)      base = base.darker (0.2f);
        else if (isOver) base = base.brighter (0.1f);

        g.setColour (base);
        g.fillRoundedRectangle (bounds, cornerRadius);

        g.setColour (base.contrasting (0.35f));
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);
    }

private:
    float cornerRadius = 6.0f;
};
