#include "EmbeddedSampleCatalog.h"
#include "BinaryData.h"

#include <algorithm>

namespace EmbeddedSamples
{
namespace
{
    bool isAudioFilename(const juce::String& filename)
    {
        auto ext = filename.fromLastOccurrenceOf(".", false, false);
        if (ext.isEmpty())
            return false;

        ext = ext.toLowerCase();
        return ext == "wav" || ext == "aiff" || ext == "aif" || ext == "flac";
    }

    juce::String trimExtension(const juce::String& name)
    {
        auto index = name.lastIndexOfChar('.');
        if (index <= 0)
            return name.trim();

        return name.substring(0, index).trim();
    }

    std::vector<SampleInfo> buildSampleList()
    {
        std::vector<SampleInfo> list;
        list.reserve(BinaryData::namedResourceListSize);

        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            const auto* resourceName = BinaryData::namedResourceList[i];
            const auto* originalName = BinaryData::originalFilenames[i];
            if (resourceName == nullptr || originalName == nullptr)
                continue;

            const juce::String originalFilename(originalName);
            if (!isAudioFilename(originalFilename))
                continue;

            SampleInfo info;
            info.resourceName = resourceName;
            info.originalFilename = originalFilename;

            const int dashIndex = originalFilename.indexOfChar('-');
            if (dashIndex > 0)
            {
                info.category = originalFilename.substring(0, dashIndex).trim();
                auto remainder = originalFilename.substring(dashIndex + 1).trim();
                info.displayName = trimExtension(remainder);
            }
            else
            {
                info.category = "Other";
                info.displayName = trimExtension(originalFilename);
            }

            list.push_back(std::move(info));
        }

        std::sort(list.begin(), list.end(), [](const SampleInfo& a, const SampleInfo& b)
        {
            const auto categoryCompare = a.category.compareIgnoreCase(b.category);
            if (categoryCompare != 0)
                return categoryCompare < 0;

            return a.displayName.compareIgnoreCase(b.displayName) < 0;
        });

        return list;
    }
}

    const std::vector<SampleInfo>& getAllSamples()
    {
        static const std::vector<SampleInfo> samples = buildSampleList();
        return samples;
    }

    const SampleInfo* findByOriginalFilename(const juce::String& originalFilename)
    {
        if (originalFilename.isEmpty())
            return nullptr;

        const auto& all = getAllSamples();
        for (const auto& sample : all)
            if (sample.originalFilename.equalsIgnoreCase(originalFilename))
                return &sample;

        return nullptr;
    }

    const SampleInfo* findByResourceName(const juce::String& resourceName)
    {
        if (resourceName.isEmpty())
            return nullptr;

        const auto& all = getAllSamples();
        for (const auto& sample : all)
            if (sample.resourceName == resourceName)
                return &sample;

        return nullptr;
    }
}

