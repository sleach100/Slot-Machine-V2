#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <memory>
#include <functional>
#include <deque>
#include <vector>
#include <utility>
#include <map>
#include "PluginProcessor.h"

class PolyrhythmVizComponent;

class SlotMachineAudioProcessorEditor
    : public juce::AudioProcessorEditor
    , private juce::Button::Listener
    , private juce::Timer
    , private juce::AudioProcessorValueTreeState::Listener
{
public:

    using APVTS = juce::AudioProcessorValueTreeState;

    SlotMachineAudioProcessorEditor(SlotMachineAudioProcessor& processor,
        APVTS& state);
    ~SlotMachineAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Standalone helper used to ensure the UI starts in a clean state.
    void resetUiToDefaultStateForStandalone();


private:
    struct EmbeddedSample
    {
        juce::String category;
        juce::String display;
        juce::String resourceName;
    };

    using EmbeddedCatalog = std::map<juce::String, juce::Array<EmbeddedSample>>;

    class VisualizerWindow;

    class PatternTabs : public juce::Component,
                        private juce::Button::Listener
    {
    public:
        PatternTabs();

        void setTabs(const juce::StringArray& names);
        void setCurrentIndex(int index, bool notify = false);
        int  getCurrentIndex() const { return currentIndex; }
        juce::Rectangle<int> getTabBoundsInParent(int index) const;

        void onTabSelected(std::function<void(int)> handler) { tabSelected = std::move(handler); }
        void onTabBarRightClick(std::function<void(const juce::MouseEvent&)> handler) { rightClick = std::move(handler); }
        void onTabReordered(std::function<void(int, int)> handler) { tabReordered = std::move(handler); }

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent& e) override;
    private:
        struct TabButton : juce::TextButton
        {
            explicit TabButton(PatternTabs& ownerRef)
                : owner(ownerRef)
            {
            }

            void mouseDown(const juce::MouseEvent& e) override
            {
                owner.handleTabMouseDown(*this, e);
                juce::TextButton::mouseDown(e);
            }

            void mouseDrag(const juce::MouseEvent& e) override
            {
                owner.handleTabMouseDrag(*this, e);
                juce::TextButton::mouseDrag(e);
            }

            void mouseUp(const juce::MouseEvent& e) override
            {
                owner.handleTabMouseUp(*this, e);
                juce::TextButton::mouseUp(e);
            }

            PatternTabs& owner;
            int index = -1;
        };

        void buttonClicked(juce::Button* b) override;
        void updateToggleStates();
        void reorderTab(int fromIndex, int toIndex, bool notify);
        int  getDropIndexForPosition(int x) const;
        void handleTabMouseDown(TabButton& button, const juce::MouseEvent& e);
        void handleTabMouseDrag(TabButton& button, const juce::MouseEvent& e);
        void handleTabMouseUp(TabButton& button, const juce::MouseEvent& e);
        void resetDragState(bool clearSuppressed = true);

        juce::OwnedArray<TabButton> buttons;
        int currentIndex = 0;
        std::function<void(int)> tabSelected;
        std::function<void(const juce::MouseEvent&)> rightClick;
        std::function<void(int, int)> tabReordered;

        TabButton* dragButton = nullptr;
        int dragStartIndex = -1;
        int dragCurrentIndex = -1;
        int dragStartScreenX = 0;
        bool dragging = false;
        bool suppressNextClick = false;
    };

    class RenamePatternComponent : public juce::Component,
                                   private juce::Button::Listener,
                                   private juce::TextEditor::Listener
    {
    public:
        using ResultHandler = std::function<void(bool accepted, juce::String newName)>;

        RenamePatternComponent(const juce::String& currentName, ResultHandler handler);
        ~RenamePatternComponent() override;

        void setCallOutBox(juce::CallOutBox& box);
        void focusEditor();

        void resized() override;

    private:
        void buttonClicked(juce::Button* button) override;
        void textEditorReturnKeyPressed(juce::TextEditor&) override;
        void textEditorEscapeKeyPressed(juce::TextEditor&) override;

        void commit(bool accepted);

        juce::Label prompt;
        juce::TextEditor editor;
        juce::TextButton okButton{ "OK" };
        juce::TextButton cancelButton{ "Cancel" };

        ResultHandler onResult;
        juce::CallOutBox* owner = nullptr;
        bool hasCommitted = false;
    };

    // ===== Master UI =====
    void mouseUp(const juce::MouseEvent& e) override;  // <-- this line
    void handleMasterTap();
    juce::TooltipWindow tooltipWindow;  // must live as long as the editor
    juce::Image        logoImage;

    juce::Label  masterLabel{ {}, "Master BPM" };
    juce::Slider masterBPM;
    juce::ToggleButton startToggle; // hidden, just for attachment

    juce::TextButton btnStart{ "Start" };
    juce::TextButton btnSave{ "Save" };
    juce::TextButton btnLoad{ "Load" };
    juce::TextButton btnInitialize{ "Initialize" };
    juce::TextButton btnResetLoop{ "Reset Loop" };
    juce::TextButton btnReset{ "Reset UI" };
    juce::TextButton btnOptions{ "Options" };
    juce::TextButton btnExportMidi{ "Export MIDI" };
    juce::TextButton btnExportAudio{ "Export Audio" };
    juce::TextButton btnVisualizer{ "Visualize" };
    juce::TextButton btnTutorial{ "Tutorial" };
    juce::TextButton btnUserManual{ "User Manual" };
    juce::TextButton btnAbout{ "About" };

    std::unique_ptr<APVTS::SliderAttachment> masterBPMA;
    std::unique_ptr<APVTS::ButtonAttachment> masterRunA;

    juce::Rectangle<int> masterBarBounds;
    juce::Rectangle<int> logoBounds;
    float masterPhase = 0.0f;

    std::deque<double> masterTapTimes;

    float lastPhase = 0.0f;      // previous cycle phase (0..1)
    float cycleFlash = 0.0f;     // 0..1 envelope that decays after wrap
    bool lastShowVisualizer = false;

    std::unique_ptr<juce::DocumentWindow> vizWindow;
    std::unique_ptr<PolyrhythmVizComponent> vizComponent;

    // ===== Slot UI =====
    struct SlotUI
    {
        struct ToggleLabel : juce::Label
        {
            ToggleLabel()
            {
                setInterceptsMouseClicks(true, false);
            }

            void mouseUp(const juce::MouseEvent& e) override
            {
                juce::Label::mouseUp(e);

                if (target != nullptr && e.mouseWasClicked())
                    target->triggerClick();
            }

            juce::Button* target = nullptr;
        };

        struct FileButton : juce::TextButton, juce::FileDragAndDropTarget
        {
            FileButton();

            std::function<void(const juce::File&)> onFileDropped;

            bool isInterestedInFileDrag(const juce::StringArray& files) override;
            void fileDragEnter(const juce::StringArray& files, int, int) override;
            void fileDragExit(const juce::StringArray& files) override;
            void filesDropped(const juce::StringArray& files, int, int) override;

        private:
            void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;
            static bool containsSupportedFile(const juce::StringArray& files);
            static bool isSupportedFile(const juce::File& file);
            void updateDragHighlight(bool shouldHighlight);

            bool dragActive = false;
        };

        juce::GroupComponent group;
        FileButton        fileBtn;
        juce::TextButton  clearBtn{ "X" };      // NEW: Clear sample
        juce::Label       fileLabel;

        juce::ImageButton muteBtn;
        juce::ImageButton soloBtn;
        ToggleLabel       muteLabel;
        ToggleLabel       soloLabel;
        juce::ComboBox    midiChannel;

        juce::Slider count, rate, gain, decay;

        void updateTimingModeVisibility(int timingMode);

        std::unique_ptr<APVTS::ButtonAttachment> muteA, soloA;
        std::unique_ptr<APVTS::SliderAttachment> countA, rateA, gainA, decayA;
        std::unique_ptr<APVTS::ComboBoxAttachment> midiChannelA;

        bool     hasFile = false;
        float    glow = 0.0f;
        float    phase = 0.0f;
        uint32_t lastHitCounter = 0;

        int titleLabelRaiseOffset = 0;

        bool syncingFromRate = false;
        bool syncingFromCount = false;
        bool beatsQuickPickExpanded = false;

        bool showRateLabel = true;
        bool showCountLabel = false;
    };

    static constexpr int kNumSlots = SlotMachineAudioProcessor::kNumSlots;
    static constexpr int kMaxBeatsPerSlot = 64;
    std::array<std::unique_ptr<SlotUI>, kNumSlots> slots;
    std::array<juce::String, kNumSlots> embeddedSlotResourceNames{};

    PatternTabs patternTabs;
    juce::Label patternWarningLabel;

    // ===== Helpers =====
    void buttonClicked(juce::Button*) override;
    void openUserManual();
    void timerCallback() override;
    void updateStandaloneWindowTitle();

    void setMasterRun(bool shouldRun);
    void updateStartButtonVisuals(bool shouldRun,
        juce::Colour glowColour,
        juce::Colour pulseColour,
        float glowAlpha,
        float glowWidth);
    void animateStartButton(juce::Colour glowColour, juce::Colour pulseColour);
    void updateSliderKnobColours(juce::Colour pulseColour);
    void doSavePreset();
    void doLoadPreset();
    void doResetAll(bool persistOptions = true);
    void resetLoopTransport();
    void resetProgressVisuals();
    void showOptionsDialog();
    void promptForExportCycles(const juce::String& dialogTitle,
        int defaultCycles,
        std::function<void(int)> onConfirm);
    void beginAudioExportWithCycles(int cyclesRequested);
    void beginMidiExportWithCycles(int cyclesRequested);
    void openVisualizerWindow();
    void closeVisualizerWindow();
    void handleVisualizerWindowCloseRequest();
    void setShowVisualizerParam(bool shouldShow);
    void handleSlotRateChanged(int slotIndex, SlotUI& ui);
    void handleSlotCountChanged(int slotIndex, SlotUI& ui);
    void initialiseSlotTimingPair(int slotIndex, SlotUI& ui);
    static int convertRateToCount(float rateValue)
    {
        const float clampedRate = juce::jlimit(0.0625f, 4.0f, rateValue);
        const int scaled = juce::jmax(1, juce::roundToInt(clampedRate * (float)SlotMachineAudioProcessor::kCountModeBaseBeats));
        return juce::jlimit(1, kMaxBeatsPerSlot, scaled);
    }

    static float convertCountToRate(int countValue)
    {
        const int clampedCount = juce::jlimit(1, kMaxBeatsPerSlot, countValue);
        const float rate = (float)clampedCount / (float)SlotMachineAudioProcessor::kCountModeBaseBeats;
        return juce::jlimit(0.0625f, 4.0f, rate);
    }
    void handleSlotFileSelection(int slotIndex, const juce::File& file);
    void initialisePatterns();
    void saveCurrentPattern();
    void refreshPatternTabs();
    void applyPattern(int index, bool updateTabs = true, bool saveExisting = true, bool deferIfRunning = false);
    void applyPatternTreeNow(const juce::ValueTree& pattern, bool allowTailRelease = false);
    void showPatternWarning(const juce::Array<int>& failedSlots);
    void refreshSlotFileLabels(const juce::Array<int>& failedSlots);
    juce::String defaultPatternNameForIndex(int index) const;
    void handlePatternContextMenu(const juce::MouseEvent& e);
    void reorderPatterns(int fromIndex, int toIndex);
    void createNewPattern();
    void duplicateCurrentPattern();
    void deleteCurrentPattern();
    void renameCurrentPattern();
    void importPatternFromFile();
    void handlePatternImportFile(const juce::File& file);
    void importPatternIntoCurrentTab(const juce::ValueTree& patternTree);
    void clearExtraPatternsBeforeLoad();
    void resetPatternsToSingleDefault();

    void applySlotScale(float newScale);
    void refreshSizeForSlotScale();
    int scaleDimension(int base) const;
    int scaleDimensionWithMax(int base, float maxScale) const;
    void refreshSlotTimingModeUI();
    void refreshSlotTimingModeUI(int timingMode);
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void buildEmbeddedSampleCatalog();
    juce::String getEmbeddedSampleDisplay(const juce::String& resourceName) const;

    // Refs
    SlotMachineAudioProcessor& processor;
    APVTS& apvts;

    EmbeddedCatalog embeddedCatalog;
    std::map<juce::String, EmbeddedSample> embeddedSampleLookup;
    bool embeddedCatalogBuilt = false;

    float slotScale = 1.0f;
    int   lastTimingMode = 0;

    bool lastStartToggleState = false;
    float startButtonAnimPhase = 0.0f;
    juce::GlowEffect startButtonGlow;
    bool startButtonGlowEnabled = false;
    juce::Colour cachedStartGlowColour{ juce::Colours::transparentBlack };
    juce::Colour cachedStartPulseColour{ juce::Colours::transparentBlack };
    float cachedStartGlowAlpha = -1.0f;
    float cachedStartGlowWidth = -1.0f;
    juce::Colour cachedKnobPulseColour{ juce::Colours::transparentBlack };

    juce::ValueTree patternsTree;
    int currentPatternIndex = 0;

    bool patternSwitchPending = false;
    juce::ValueTree pendingPatternTree;
    bool fileDialogActive = false;
    juce::Component::SafePointer<juce::DialogWindow> exportCyclesPromptWindow;
    juce::Component::SafePointer<juce::DialogWindow> aboutDialog;
    int patternWarningCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotMachineAudioProcessorEditor)
};
