#pragma once

#if defined(__has_include)
#if __has_include(<JuceHeader.h>)
#include <JuceHeader.h>
#elif __has_include("JuceHeader.h")
#include "JuceHeader.h"
#elif __has_include("../JuceLibraryCode/JuceHeader.h")
#include "../JuceLibraryCode/JuceHeader.h"
#elif __has_include(<juce_audio_basics/juce_audio_basics.h>)
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#elif __has_include("../JuceLibraryCode/modules/juce_audio_basics/juce_audio_basics.h")
#include "../JuceLibraryCode/modules/juce_audio_basics/juce_audio_basics.h"
#include "../JuceLibraryCode/modules/juce_core/juce_core.h"
#else
#error "Unable to locate JUCE headers for WaveformUtils"
#endif
#else
#include <JuceHeader.h>
#endif

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

// Small fixed-size mono blocks to push from audio thread
// Single-producer (audio thread) / single-consumer (message thread)
// queue that stores BlockSize-sample chunks.
template <int BlockSize, int NumBlocks>
class AudioBlockQueue
{
public:
    AudioBlockQueue()
        : fifo (NumBlocks)
    {
        static_assert(BlockSize > 0, "BlockSize must be positive");
        static_assert(NumBlocks > 0, "NumBlocks must be positive");

        for (auto& block : blocks)
        {
            block.setSize(1, BlockSize);
            block.clear();
        }
    }

    // audio thread: push up to BlockSize mono samples
    void push (const float* mono, int num) noexcept
    {
        if (mono == nullptr || num <= 0)
            return;

        while (num > 0)
        {
            if (writePos == 0)
                blocks[(size_t) currentWrite].clear();

            const int toCopy = juce::jmin (num, BlockSize - writePos);
            juce::FloatVectorOperations::copy (blocks[(size_t) currentWrite].getWritePointer (0) + writePos,
                                               mono,
                                               toCopy);

            writePos += toCopy;
            mono     += toCopy;
            num      -= toCopy;

            if (writePos >= BlockSize)
            {
                writePos = 0;

                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToWrite (1, start1, size1, start2, size2);

                if (size1 > 0)
                {
                    readyIndices[(size_t) start1] = currentWrite;
                    fifo.finishedWrite (size1);
                }
                else if (size2 > 0)
                {
                    readyIndices[(size_t) start2] = currentWrite;
                    fifo.finishedWrite (size2);
                }

                currentWrite = (currentWrite + 1) % NumBlocks;
            }
        }
    }

    // message thread: pop available blocks into dest (returns blocks copied)
    int popTo (juce::AudioBuffer<float>& dest, int startSample)
    {
        if (dest.getNumChannels() == 0)
            return 0;

        int copiedBlocks = 0;

        while (true)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (1, start1, size1, start2, size2);

            if (size1 == 0 && size2 == 0)
                break;

            if (size1 > 0)
            {
                const int index = readyIndices[(size_t) start1];
                dest.copyFrom (0, startSample, blocks[(size_t) index], 0, 0, BlockSize);
                startSample += BlockSize;
                ++copiedBlocks;
                fifo.finishedRead (size1);
                continue;
            }

            if (size2 > 0)
            {
                const int index = readyIndices[(size_t) start2];
                dest.copyFrom (0, startSample, blocks[(size_t) index], 0, 0, BlockSize);
                startSample += BlockSize;
                ++copiedBlocks;
                fifo.finishedRead (size2);
            }
        }

        return copiedBlocks;
    }

    void reset() noexcept
    {
        fifo.reset();
        writePos = 0;
        currentWrite = 0;

        for (auto& block : blocks)
            block.clear();
    }

    constexpr static int getBlockSize()          { return BlockSize; }
    constexpr static int getCapacitySamples()    { return BlockSize * NumBlocks; }

private:
    juce::AbstractFifo fifo;
    std::array<juce::AudioBuffer<float>, NumBlocks> blocks;
    std::array<int, NumBlocks> readyIndices{};
    int currentWrite = 0;
    int writePos = 0;
};

// Bins time-domain samples to screen columns using min/max per pixel (cheap & pretty)
struct MinMaxBinner
{
    static void compute (const float* samples,
                         int numSamples,
                         float* outMin,
                         float* outMax,
                         int numColumns)
    {
        if (samples == nullptr || numSamples <= 0 || numColumns <= 0)
        {
            if (outMin != nullptr)
                juce::FloatVectorOperations::clear (outMin, numColumns);
            if (outMax != nullptr)
                juce::FloatVectorOperations::clear (outMax, numColumns);
            return;
        }

        const double ratio = (double) numSamples / (double) juce::jmax (1, numColumns);

        for (int x = 0; x < numColumns; ++x)
        {
            const int start = (int) std::floor ((double) x * ratio);
            const int end   = juce::jmin (numSamples, (int) std::floor (((double) x + 1.0) * ratio));

            float mn =  std::numeric_limits<float>::max();
            float mx = std::numeric_limits<float>::lowest();

            for (int i = start; i < end; ++i)
            {
                const float s = samples[i];
                if (s < mn) mn = s;
                if (s > mx) mx = s;
            }

            if (start == end)
            {
                mn = 0.0f;
                mx = 0.0f;
            }
            else
            {
                if (! std::isfinite (mn)) mn = 0.0f;
                if (! std::isfinite (mx)) mx = 0.0f;
            }

            outMin[x] = juce::jlimit (-1.0f, 1.0f, mn);
            outMax[x] = juce::jlimit (-1.0f, 1.0f, mx);
        }
    }
};

