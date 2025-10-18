#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterApp.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

// Custom window that disables input by default
class CustomStandaloneWindow : public juce::StandaloneFilterWindow
{
public:
    CustomStandaloneWindow()
        : StandaloneFilterWindow(JucePlugin_Name,
                                 juce::Colours::black,
                                 juce::DocumentWindow::allButtons,
                                 true)
    {
    }

    void initialise() override
    {
        StandaloneFilterWindow::initialise();

        // Force input device off
        auto& deviceManager = getDeviceManager();
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);

        setup.inputDeviceName = {};            // disable input
        setup.useDefaultInputChannels = false; // no input channels
        setup.useDefaultOutputChannels = true; // keep output

        deviceManager.setAudioDeviceSetup(setup, true);

        if (auto* processor = dynamic_cast<SlotMachineAudioProcessor*>(getAudioProcessor()))
        {
            if (auto* editor = dynamic_cast<SlotMachineAudioProcessorEditor*>(processor->getActiveEditor()))
            {
                editor->resetUiToDefaultStateForStandalone();
            }
        }
    }
};

// Custom standalone application
class CustomStandaloneApplication : public juce::StandaloneFilterApp
{
public:
    std::unique_ptr<juce::StandaloneFilterWindow> createMainWindow() override
    {
        return std::make_unique<CustomStandaloneWindow>();
    }
};

// Replace JUCE’s default entry point
START_JUCE_APPLICATION (CustomStandaloneApplication)
