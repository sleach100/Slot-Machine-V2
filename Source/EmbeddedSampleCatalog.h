#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace EmbeddedSamples
{
    struct SampleInfo
    {
        juce::String resourceName;
        juce::String originalFilename;
        juce::String category;
        juce::String displayName;
    };

    const std::vector<SampleInfo>& getAllSamples();
    const SampleInfo* findByOriginalFilename(const juce::String& originalFilename);
    const SampleInfo* findByResourceName(const juce::String& resourceName);
}

