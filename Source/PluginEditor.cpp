#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "PolyrhythmVizComponent.h"
#include "BeatsQuickPickGrid.h"

#include <memory>
#include <cmath>
#include <array>
#include <limits>
#include <functional>

#if __has_include("BinaryData.h")
#include "BinaryData.h"
#else
namespace BinaryData
{
    extern const unsigned char MuteOFF_png[];
    extern const int MuteOFF_pngSize;
    extern const unsigned char MuteON_png[];
    extern const int MuteON_pngSize;
    extern const unsigned char SoloOFF_png[];
    extern const int SoloOFF_pngSize;
    extern const unsigned char SoloON_png[];
    extern const int SoloON_pngSize;
    extern const char* SlotMachineUserManual_html;
    extern const int   SlotMachineUserManual_htmlSize;
}
#endif

using APVTS = juce::AudioProcessorValueTreeState;
static const juce::Identifier kPatternNameProperty("name");

static juce::Font createBoldFont(float size);

namespace
{
    template <typename Group>
    auto tryGetSlotTitleLabel(Group& group, int) -> decltype(&group.getTextLabel())
    {
        return &group.getTextLabel();
    }

    template <typename Group>
    juce::Label* tryGetSlotTitleLabel(Group&, long)
    {
        return nullptr;
    }

    juce::Label* getSlotTitleLabelIfAvailable(juce::GroupComponent& group)
    {
        return tryGetSlotTitleLabel(group, 0);
    }

    constexpr int kBeatsQuickPickDefaultMax = 32;

    constexpr auto kStandaloneWindowTitle = "";// This sets the text in the title bar of the standalone app
    constexpr int kMasterControlsYOffset = 70;
    constexpr int kMasterLabelExtraYOffset = 35;
    constexpr float kBannerScaleMultiplier = 2.24f;

    void confirmWarningWithContinue(juce::Component* parent,
        const juce::String& title,
        const juce::String& message,
        std::function<void()> onConfirm)
    {
        auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle(title)
            .withMessage(message)
            .withButton("Continue")
            .withButton("Cancel");

        if (parent != nullptr)
            options = options.withAssociatedComponent(parent);

        juce::AlertWindow::showAsync(options,
            juce::ModalCallbackFunction::create([fn = std::move(onConfirm)](int result)
            {
                if (result == 1 && fn)
                    fn();
            }));
    }

    class ExportCyclesDialog : public juce::Component,
                               private juce::Button::Listener,
                               private juce::TextEditor::Listener
    {
    public:
        using ConfirmHandler = std::function<void(int)>;
        using CancelHandler = std::function<void()>;

        ExportCyclesDialog(int defaultCycles,
            ConfirmHandler onConfirmFn,
            CancelHandler onCancelFn)
            : onConfirm(std::move(onConfirmFn))
            , onCancel(std::move(onCancelFn))
        {
            instruction.setText("How many cycles would you like to export?", juce::dontSendNotification);
            instruction.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(instruction);

            cyclesLabel.setText("Cycles:", juce::dontSendNotification);
            cyclesLabel.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(cyclesLabel);

            const int initialCycles = juce::jmax(1, defaultCycles);
            cyclesEditor.setText(juce::String(initialCycles));
            cyclesEditor.setInputRestrictions(0, "0123456789");
            cyclesEditor.setJustification(juce::Justification::centredLeft);
            cyclesEditor.setSelectAllWhenFocused(true);
            cyclesEditor.addListener(this);
            addAndMakeVisible(cyclesEditor);

            errorLabel.setJustificationType(juce::Justification::centredLeft);
            errorLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
            addAndMakeVisible(errorLabel);

            okButton.addListener(this);
            okButton.setButtonText("OK");
            addAndMakeVisible(okButton);

            cancelButton.addListener(this);
            cancelButton.setButtonText("Cancel");
            addAndMakeVisible(cancelButton);
        }

        ~ExportCyclesDialog() override
        {
            if (!hasResolved)
            {
                if (auto cancelCopy = onCancel)
                    cancelCopy();
            }
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(20);

            auto messageBounds = bounds.removeFromTop(48);
            instruction.setBounds(messageBounds);

            bounds.removeFromTop(8);
            auto inputRow = bounds.removeFromTop(28);
            auto labelWidth = 90;
            cyclesLabel.setBounds(inputRow.removeFromLeft(labelWidth));
            inputRow.removeFromLeft(12);
            cyclesEditor.setBounds(inputRow.removeFromLeft(120));

            bounds.removeFromTop(6);
            errorLabel.setBounds(bounds.removeFromTop(20));

            bounds.removeFromBottom(8);
            auto buttonsArea = bounds.removeFromBottom(32);
            auto rightSection = buttonsArea.removeFromRight(180);
            okButton.setBounds(rightSection.removeFromRight(80));
            rightSection.removeFromRight(16);
            cancelButton.setBounds(rightSection.removeFromRight(80));
        }

        void visibilityChanged() override
        {
            if (isVisible())
            {
                cyclesEditor.grabKeyboardFocus();
                cyclesEditor.selectAll();
            }
        }

    private:
        void buttonClicked(juce::Button* button) override
        {
            if (button == &okButton)
                handleOk();
            else if (button == &cancelButton)
                handleCancel();
        }

        void textEditorReturnKeyPressed(juce::TextEditor&) override
        {
            handleOk();
        }

        void textEditorEscapeKeyPressed(juce::TextEditor&) override
        {
            handleCancel();
        }

        void textEditorTextChanged(juce::TextEditor&) override
        {
            errorLabel.setText({}, juce::dontSendNotification);
        }

        void handleOk()
        {
            const auto text = cyclesEditor.getText().trim();
            if (text.isEmpty())
            {
                showError();
                return;
            }

            const int cycles = text.getIntValue();
            if (cycles <= 0)
            {
                showError();
                return;
            }

            hasResolved = true;

            auto confirmCopy = onConfirm;
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                window->exitModalState(1);

            if (confirmCopy != nullptr)
            {
                juce::MessageManager::callAsync([confirmCopy, cycles]() mutable
                {
                    confirmCopy(cycles);
                });
            }
        }

        void handleCancel()
        {
            hasResolved = true;

            auto cancelCopy = onCancel;
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                window->exitModalState(0);

            if (cancelCopy != nullptr)
                cancelCopy();
        }

        void showError()
        {
            errorLabel.setText("Please enter a positive whole number of cycles.", juce::dontSendNotification);
            cyclesEditor.grabKeyboardFocus();
            cyclesEditor.selectAll();
        }

        juce::Label instruction;
        juce::Label cyclesLabel;
        juce::TextEditor cyclesEditor;
        juce::Label errorLabel;
        juce::TextButton okButton;
        juce::TextButton cancelButton;

        ConfirmHandler onConfirm;
        CancelHandler onCancel;
        bool hasResolved = false;
    };

    class AboutComponent : public juce::Component
    {
    public:
        AboutComponent()
        {
            logo = juce::ImageCache::getFromMemory(BinaryData::LonePearLogic_png,
                                                   BinaryData::LonePearLogic_pngSize);

            logoComponent.setImage(logo,
                                   juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            addAndMakeVisible(logoComponent);

            aboutLabel.setText("Slot Machine by Lone Pear Logic.  Copyright 2025.",
                               juce::dontSendNotification);
            aboutLabel.setJustificationType(juce::Justification::centred);
            aboutLabel.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
            aboutLabel.setFont(createBoldFont(16.0f));
            addAndMakeVisible(aboutLabel);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(20);

            const int labelHeight = 48;
            auto imageArea = bounds.removeFromTop(juce::jmax(120, bounds.getHeight() - labelHeight - 20));
            logoComponent.setBounds(imageArea);

            bounds.removeFromTop(20);
            aboutLabel.setBounds(bounds.removeFromTop(labelHeight));
        }

    private:
        juce::Image logo;
        juce::ImageComponent logoComponent;
        juce::Label aboutLabel;
    };
}

// ===== PatternTabs =====
SlotMachineAudioProcessorEditor::PatternTabs::PatternTabs()
{
    setInterceptsMouseClicks(true, true);
}

void SlotMachineAudioProcessorEditor::PatternTabs::setTabs(const juce::StringArray& names)
{
    resetDragState();

    for (int i = buttons.size(); --i >= 0;)
    {
        if (auto* button = buttons.getUnchecked(i))
        {
            button->removeListener(this);
            removeChildComponent(button);
        }
    }

    buttons.clear();

    for (int i = 0; i < names.size(); ++i)
    {
        auto* button = new TabButton(*this);
        button->index = i;
        button->setButtonText(names[i]);
        button->setClickingTogglesState(true);
        button->setRadioGroupId(1);
        button->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        button->setColour(juce::TextButton::buttonOnColourId, juce::Colours::dimgrey);
        button->setColour(juce::TextButton::textColourOffId, juce::Colours::whitesmoke);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        button->addListener(this);
        addAndMakeVisible(button);
        buttons.add(button);
    }

    currentIndex = juce::jlimit(0, juce::jmax(0, buttons.size() - 1), currentIndex);
    updateToggleStates();
    resized();
    repaint();
}

void SlotMachineAudioProcessorEditor::PatternTabs::setCurrentIndex(int index, bool notify)
{
    if (buttons.isEmpty())
    {
        currentIndex = 0;
        return;
    }

    index = juce::jlimit(0, buttons.size() - 1, index);

    if (currentIndex == index)
        return;

    currentIndex = index;
    updateToggleStates();

    if (notify && tabSelected)
        tabSelected(currentIndex);
}

juce::Rectangle<int> SlotMachineAudioProcessorEditor::PatternTabs::getTabBoundsInParent(int index) const
{
    if (juce::isPositiveAndBelow(index, buttons.size()))
    {
        if (auto* button = buttons[index])
            return button->getBoundsInParent().translated(getX(), getY());
    }

    return getBounds();
}

void SlotMachineAudioProcessorEditor::PatternTabs::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.drawRoundedRectangle(bounds, 6.0f, 1.2f);
}

void SlotMachineAudioProcessorEditor::PatternTabs::resized()
{
    const int count = buttons.size();
    if (count <= 0)
        return;

    auto area = getLocalBounds();
    const int baseWidth = area.getWidth() / count;
    int remainder = area.getWidth() - baseWidth * count;
    int x = area.getX();

    for (int i = 0; i < count; ++i)
    {
        int w = baseWidth;
        if (remainder > 0)
        {
            ++w;
            --remainder;
        }

        if (auto* button = buttons[i])
            button->setBounds(x, area.getY(), w, area.getHeight());
        x += w;
    }
}

void SlotMachineAudioProcessorEditor::PatternTabs::handleTabMouseDown(TabButton& button, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    resetDragState();

    dragButton = &button;
    dragStartIndex = dragButton->index;
    dragCurrentIndex = dragStartIndex;
    dragStartScreenX = e.getScreenX();
    dragging = false;
}

void SlotMachineAudioProcessorEditor::PatternTabs::handleTabMouseDrag(TabButton& button, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    if (&button != dragButton)
        return;

    if (buttons.size() <= 1)
        return;

    const int delta = e.getScreenX() - dragStartScreenX;
    const int distance = delta >= 0 ? delta : -delta;
    if (!dragging)
    {
        if (distance < 4)
            return;

        dragging = true;
        suppressNextClick = true;
    }

    const int localX = e.getScreenX() - getScreenX();
    const int target = getDropIndexForPosition(localX);

    if (target >= 0 && target != dragCurrentIndex)
    {
        reorderTab(dragCurrentIndex, target, false);
        dragCurrentIndex = target;
    }

}

void SlotMachineAudioProcessorEditor::PatternTabs::handleTabMouseUp(TabButton& button, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        resetDragState();

        if (button.index != currentIndex)
            setCurrentIndex(button.index, true);

        if (rightClick)
            rightClick(e);
        return;
    }

    if (dragging)
    {
        if (dragStartIndex != -1 && dragCurrentIndex != -1 && dragStartIndex != dragCurrentIndex)
        {
            if (tabReordered)
                tabReordered(dragStartIndex, dragCurrentIndex);
        }

        resetDragState(false);
        suppressNextClick = true;
        return;
    }

    resetDragState(false);
}

void SlotMachineAudioProcessorEditor::PatternTabs::mouseUp(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu())
        return;

    resetDragState();

    const juce::Point<int> pos(e.getScreenX() - getScreenX(),
                               e.getScreenY() - getScreenY());

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* buttonAtPos = buttons[i])
        {
            if (buttonAtPos->getBoundsInParent().contains(pos))
            {
                if (buttonAtPos->index != currentIndex)
                    setCurrentIndex(buttonAtPos->index, true);

                if (rightClick)
                    rightClick(e);
                return;
            }
        }
    }

    if (rightClick)
        rightClick(e);
}

void SlotMachineAudioProcessorEditor::PatternTabs::buttonClicked(juce::Button* b)
{
    if (suppressNextClick)
    {
        suppressNextClick = false;
        return;
    }

    auto* tabButton = dynamic_cast<TabButton*>(b);
    if (tabButton == nullptr)
        return;

    if (tabSelected)
        tabSelected(tabButton->index);
}

void SlotMachineAudioProcessorEditor::PatternTabs::updateToggleStates()
{
    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* button = buttons[i])
            button->setToggleState(button->index == currentIndex, juce::dontSendNotification);
    }
}

void SlotMachineAudioProcessorEditor::PatternTabs::reorderTab(int fromIndex, int toIndex, bool notify)
{
    if (fromIndex == toIndex)
        return;

    if (!juce::isPositiveAndBelow(fromIndex, buttons.size())
        || !juce::isPositiveAndBelow(toIndex, buttons.size()))
        return;

    auto* button = buttons[fromIndex];
    buttons.remove(fromIndex, false);
    buttons.insert(toIndex, button);

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* b = buttons[i])
            b->index = i;
    }

    if (currentIndex == fromIndex)
        currentIndex = toIndex;
    else if (currentIndex > fromIndex && currentIndex <= toIndex)
        --currentIndex;
    else if (currentIndex < fromIndex && currentIndex >= toIndex)
        ++currentIndex;

    updateToggleStates();
    resized();
    repaint();

    if (notify && tabReordered)
        tabReordered(fromIndex, toIndex);
}

int SlotMachineAudioProcessorEditor::PatternTabs::getDropIndexForPosition(int x) const
{
    if (buttons.isEmpty())
        return -1;

    int clampedX = juce::jlimit(0, getWidth(), x);
    int result = buttons.size() - 1;

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (auto* button = buttons[i])
        {
            const int boundary = button->getBounds().getCentreX();
            if (clampedX < boundary)
            {
                result = i;
                break;
            }
        }
    }

    return result;
}

void SlotMachineAudioProcessorEditor::PatternTabs::resetDragState(bool clearSuppressed)
{
    dragButton = nullptr;
    dragStartIndex = -1;
    dragCurrentIndex = -1;
    dragStartScreenX = 0;
    dragging = false;

    if (clearSuppressed)
        suppressNextClick = false;
}

// ===== RenamePatternComponent =====
SlotMachineAudioProcessorEditor::RenamePatternComponent::RenamePatternComponent(const juce::String& currentName,
    ResultHandler handler)
    : prompt({}, "Enter a new name:"),
      onResult(std::move(handler))
{
    prompt.setJustificationType(juce::Justification::centredLeft);
    prompt.setFont(juce::Font(15.0f, juce::Font::bold));
    addAndMakeVisible(prompt);

    editor.setSelectAllWhenFocused(true);
    editor.setText(currentName, juce::dontSendNotification);
    editor.addListener(this);
    addAndMakeVisible(editor);

    okButton.addListener(this);
    cancelButton.addListener(this);
    addAndMakeVisible(okButton);
    addAndMakeVisible(cancelButton);

    setSize(260, 110);
}

SlotMachineAudioProcessorEditor::RenamePatternComponent::~RenamePatternComponent()
{
    if (!hasCommitted && onResult)
    {
        auto handler = std::move(onResult);
        handler(false, editor.getText());
    }
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::setCallOutBox(juce::CallOutBox& box)
{
    owner = &box;
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::focusEditor()
{
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<RenamePatternComponent>(this)]
    {
        if (safe != nullptr)
        {
            safe->editor.grabKeyboardFocus();
            safe->editor.selectAll();
        }
    });
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::resized()
{
    auto area = getLocalBounds().reduced(12);

    prompt.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);

    editor.setBounds(area.removeFromTop(28));
    area.removeFromTop(12);

    auto buttonsArea = area.removeFromTop(28);
    auto ok = buttonsArea.removeFromLeft(buttonsArea.getWidth() / 2).reduced(4, 0);
    auto cancel = buttonsArea.reduced(4, 0);
    okButton.setBounds(ok);
    cancelButton.setBounds(cancel);
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::buttonClicked(juce::Button* button)
{
    if (button == &okButton)
    {
        commit(true);
    }
    else if (button == &cancelButton)
    {
        commit(false);
    }
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::textEditorReturnKeyPressed(juce::TextEditor&)
{
    commit(true);
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::textEditorEscapeKeyPressed(juce::TextEditor&)
{
    commit(false);
}

void SlotMachineAudioProcessorEditor::RenamePatternComponent::commit(bool accepted)
{
    if (hasCommitted)
        return;

    hasCommitted = true;
    auto handler = std::move(onResult);

    if (handler)
        handler(accepted, editor.getText());

    if (owner != nullptr)
    {
        owner->dismiss();
        owner = nullptr;
    }
}

// ===== Font helpers =====
static juce::Font createBoldFont(float size)
{
    juce::Font f{ size };
    f.setBold(true);
    return f;
}

// ===== Knob helper =====
static juce::Slider& setupKnob(juce::Slider& s,
    double min, double max, double inc,
    const juce::String& name,
    int numDecimals = -1)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 18);
    s.setRange(min, max, inc);
    s.setName(name);

    if (numDecimals >= 0)
        s.setNumDecimalPlacesToDisplay(numDecimals);

    return s;
}

SlotMachineAudioProcessorEditor::SlotUI::FileButton::FileButton()
    : juce::TextButton("Load")
{
}

bool SlotMachineAudioProcessorEditor::SlotUI::FileButton::isInterestedInFileDrag(const juce::StringArray& files)
{
    return containsSupportedFile(files);
}

void SlotMachineAudioProcessorEditor::SlotUI::FileButton::fileDragEnter(const juce::StringArray& files, int, int)
{
    updateDragHighlight(containsSupportedFile(files));
}

void SlotMachineAudioProcessorEditor::SlotUI::FileButton::fileDragExit(const juce::StringArray& files)
{
    juce::ignoreUnused(files);
    updateDragHighlight(false);
}

void SlotMachineAudioProcessorEditor::SlotUI::FileButton::filesDropped(const juce::StringArray& files, int, int)
{
    updateDragHighlight(false);

    if (!onFileDropped)
        return;

    for (const auto& path : files)
    {
        const juce::File file(path);
        if (isSupportedFile(file))
        {
            onFileDropped(file);
            break;
        }
    }
}

void SlotMachineAudioProcessorEditor::SlotUI::FileButton::paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    juce::TextButton::paintButton(g, isMouseOverButton || dragActive, isButtonDown);

    if (dragActive)
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        auto highlightColour = findColour(juce::TextButton::textColourOffId).withAlpha(0.85f);
        g.setColour(highlightColour);
        g.drawRoundedRectangle(bounds, 4.0f, 2.0f);
    }
}

bool SlotMachineAudioProcessorEditor::SlotUI::FileButton::containsSupportedFile(const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        if (isSupportedFile(juce::File(path)))
            return true;
    }

    return false;
}

bool SlotMachineAudioProcessorEditor::SlotUI::FileButton::isSupportedFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    auto ext = file.getFileExtension();
    if (ext.isEmpty())
        return false;

    ext = ext.trimCharactersAtStart(".").toLowerCase();

    return ext == "wav" || ext == "aiff" || ext == "aif" || ext == "flac";
}

void SlotMachineAudioProcessorEditor::SlotUI::FileButton::updateDragHighlight(bool shouldHighlight)
{
    if (dragActive == shouldHighlight)
        return;

    dragActive = shouldHighlight;
    repaint();
}

// ===== Standalone persistence for Options =====
static const juce::StringArray kOptionParamIds{
    "optShowMasterBar", "optShowSlotBars", "optShowVisualizer", "optVisualizerEdgeWalk",
    "optSampleRate", "optTimingMode",
    "optSlotScale",
    "optGlowColor", "optGlowAlpha", "optGlowWidth",
    "optPulseColor", "optPulseAlpha", "optPulseWidth"
};

static bool isOptionParameter(const juce::String& paramID)
{
    return kOptionParamIds.contains(paramID);
}

static juce::File optionsFile()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(JucePlugin_Manufacturer)
        .getChildFile(JucePlugin_Name);
    dir.createDirectory();
    return dir.getChildFile("options.xml");
}

static void saveOptionsToDisk(juce::AudioProcessorValueTreeState& apvts)
{
    juce::ValueTree vt("OPTIONS");
    for (auto& id : kOptionParamIds)
    {
        if (auto* p = apvts.getParameter(id))
        {
            if (auto* b = dynamic_cast<juce::AudioParameterBool*>(p))
                vt.setProperty(id, (int)b->get(), nullptr);
            else if (auto* ip = dynamic_cast<juce::AudioParameterInt*>(p))
                vt.setProperty(id, (int)ip->get(), nullptr);
            else if (auto* fp = dynamic_cast<juce::AudioParameterFloat*>(p))
                vt.setProperty(id, (double)fp->get(), nullptr);
        }
    }
    if (auto xml = vt.createXml())
        xml->writeTo(optionsFile());
}

static void loadOptionsFromDiskIfNoHostState(juce::AudioProcessorValueTreeState& apvts)
{
    auto f = optionsFile();
    if (!f.existsAsFile()) return;

    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(f));
    if (!xml) return;

    juce::ValueTree vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid() || vt.getType() != juce::Identifier("OPTIONS")) return;

    for (auto& id : kOptionParamIds)
    {
        if (!vt.hasProperty(id)) continue;

        if (auto* p = apvts.getParameter(id))
        {
            if (auto* b = dynamic_cast<juce::AudioParameterBool*>(p))
            {
                b->beginChangeGesture();
                *b = (int)vt.getProperty(id) != 0;
                b->endChangeGesture();
            }
            else if (auto* ip = dynamic_cast<juce::AudioParameterInt*>(p))
            {
                ip->beginChangeGesture();
                *ip = (int)vt.getProperty(id);
                ip->endChangeGesture();
            }
            else if (auto* fp = dynamic_cast<juce::AudioParameterFloat*>(p))
            {
                fp->beginChangeGesture();
                *fp = (float)(double)vt.getProperty(id);
                fp->endChangeGesture();
            }
        }
    }
}

class SlotMachineAudioProcessorEditor::VisualizerWindow : public juce::DocumentWindow
{
public:
    explicit VisualizerWindow(SlotMachineAudioProcessorEditor& ownerRef)
        : juce::DocumentWindow("Polyrhythm Visualizer",
                               juce::Colours::darkgrey,
                               juce::DocumentWindow::closeButton)
        , owner(ownerRef)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setAlwaysOnTop(false);
    }

    void closeButtonPressed() override
    {
        owner.handleVisualizerWindowCloseRequest();
    }

private:
    SlotMachineAudioProcessorEditor& owner;
};

// ===== Options helpers =====
namespace Opt
{
    static inline bool getBool(APVTS& apvts, const juce::String& id, bool def)
    {
        if (auto* p = apvts.getParameter(id))
            if (auto* b = dynamic_cast<juce::AudioParameterBool*>(p))
                return b->get();
        return def;
    }
    static inline float getFloat(APVTS& apvts, const juce::String& id, float def)
    {
        if (auto* p = apvts.getParameter(id))
            if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
                return f->get();
        return def;
    }
    static inline int getInt(APVTS& apvts, const juce::String& id, int def)
    {
        if (auto* p = apvts.getParameter(id))
            if (auto* i = dynamic_cast<juce::AudioParameterInt*> (p))
                return i->get();
        return def;
    }
    static inline juce::Colour rgbParam(APVTS& apvts, const juce::String& id, int defRGB, float alpha)
    {
        const int rgb = getInt(apvts, id, defRGB);
        juce::Colour c((juce::uint8)((rgb >> 16) & 0xFF),
            (juce::uint8)((rgb >> 8) & 0xFF),
            (juce::uint8)(rgb & 0xFF));
        return c.withAlpha(juce::jlimit(0.0f, 1.0f, alpha));
    }
}

// ===== Neon frame rendering =====
namespace
{
    inline void drawNeonFrame(juce::Graphics& g,
        juce::Rectangle<float> frame,
        float cornerRadius,
        juce::Colour baseColour,
        int   layers,
        float baseThicknessPx,
        juce::Colour pulseColour,
        float pulseThicknessPx,
        float pulse)
    {
        if (baseColour.getFloatAlpha() > 0.001f && layers > 0)
        {
            for (int l = 0; l < layers; ++l)
            {
                const float t = (layers <= 1 ? 0.f : (float)l / (float)(layers - 1));
                const float a = baseColour.getFloatAlpha() * (1.0f - 0.75f * t);
                const float w = baseThicknessPx + 3.5f * t * layers;
                g.setColour(baseColour.withAlpha(a));
                g.drawRoundedRectangle(frame, cornerRadius, w);
            }
        }

        if (pulse > 0.001f && pulseColour.getFloatAlpha() > 0.001f)
        {
            const float p = juce::jlimit(0.0f, 1.0f, pulse);
            const float auraThick = juce::jlimit(0.5f, 72.0f, pulseThicknessPx);

            g.setColour(pulseColour.withAlpha(pulseColour.getFloatAlpha() * p));
            g.drawRoundedRectangle(frame, cornerRadius, auraThick);

            g.setColour(juce::Colours::white.withAlpha(0.35f * p));
            g.drawRoundedRectangle(frame.reduced(3.0f, 3.0f), cornerRadius - 2.0f, 2.0f + 2.0f * p);
        }
    }
}

// ===== Options modal component =====
// ===== Options modal component =====
class OptionsComponent : public juce::Component,
    private juce::Button::Listener,
    private juce::ChangeListener,
    private juce::Slider::Listener
{
public:
    explicit OptionsComponent(APVTS& s, std::function<void(float)> slotScaleChangedCallback = {})
        : apvts(s), slotScaleChanged(slotScaleChangedCallback)
    {
        // toggles
        addAndMakeVisible(showMasterBar);
        showMasterBar.setButtonText("Show Master BPM progress bar");
        showMasterBar.addListener(this);

        addAndMakeVisible(showSlotBars);
        showSlotBars.setButtonText("Show slot progress bars");
        showSlotBars.addListener(this);

        addAndMakeVisible(showVisualizer);
        showVisualizer.setButtonText("Show Polyrhythm Visualizer window");
        showVisualizer.addListener(this);

        addAndMakeVisible(visualizerModeLabel);
        visualizerModeLabel.setText("Visualizer Path", juce::dontSendNotification);
        visualizerModeLabel.setJustificationType(juce::Justification::centredLeft);
        visualizerModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);

        addAndMakeVisible(visualizerModeCombo);
        visualizerModeCombo.setJustificationType(juce::Justification::centredLeft);
        visualizerModeCombo.addItem("Edge Walk (perimeter)", 1);
        visualizerModeCombo.addItem("Orbit (circular)", 2);
        visualizerModeCombo.onChange = [this]() { handleVisualizerModeSelection(); };

        // sample rate
        sampleRateLabel.setText("Export Sample Rate", juce::dontSendNotification);
        sampleRateLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(sampleRateLabel);

        addAndMakeVisible(sampleRateCombo);
        sampleRateCombo.setJustificationType(juce::Justification::centredLeft);
        for (int i = 0; i < (int)sampleRateValues.size(); ++i)
        {
            const int value = sampleRateValues[(size_t)i];
            sampleRateCombo.addItem(juce::String(value) + " Hz", i + 1);
        }
        sampleRateCombo.onChange = [this]() { handleSampleRateSelection(); };

        // timing mode
        timingModeLabel.setText("Timing Mode", juce::dontSendNotification);
        timingModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(timingModeLabel);

        addAndMakeVisible(timingModeCombo);
        timingModeCombo.setJustificationType(juce::Justification::centredLeft);
        timingModeCombo.addItem("Rate (Hits/Beat)", 1);
        timingModeCombo.addItem("Beats/Cycle (Count)", 2);
        timingModeCombo.onChange = [this]() { handleTimingModeSelection(); };

        // slot scale
        slotScaleLabel.setText("Slot Row Density", juce::dontSendNotification);
        slotScaleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(slotScaleLabel);

        addAndMakeVisible(slotScaleCombo);
        slotScaleCombo.setJustificationType(juce::Justification::centredLeft);
        for (int i = 0; i < (int)slotScaleValues.size(); ++i)
        {
            const float value = slotScaleValues[(size_t)i];
            const auto label = juce::String(juce::roundToInt(value * 100.0f)) + "%";
            slotScaleCombo.addItem(label, i + 1);
        }
        slotScaleCombo.onChange = [this]() { handleSlotScaleSelection(); };

        // colour selectors
        addAndMakeVisible(glowColourSel);
        glowColourSel.setColour(juce::ColourSelector::backgroundColourId, juce::Colours::black);
        glowColourSel.setCurrentColour(defaultGlowColour());
        glowColourSel.addChangeListener(this);

        addAndMakeVisible(pulseColourSel);
        pulseColourSel.setColour(juce::ColourSelector::backgroundColourId, juce::Colours::black);
        pulseColourSel.setCurrentColour(defaultPulseColour());
        pulseColourSel.addChangeListener(this);

        glowLabel.setText("Selected Glow Colour", juce::dontSendNotification);
        glowLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(glowLabel);

        pulseLabel.setText("Pulse Colour", juce::dontSendNotification);
        pulseLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(pulseLabel);

        // sliders
        setupSlider(glowAlpha, 0.0, 1.0, 0.001, "Glow Alpha");
        setupSlider(glowWidth, 0.5, 24.0, 0.01, "Glow Width (px)");
        setupSlider(pulseAlpha, 0.0, 1.0, 0.001, "Pulse Alpha");
        setupSlider(pulseWidth, 0.5, 36.0, 0.01, "Pulse Width (px)");

        glowAlpha.addListener(this);
        glowWidth.addListener(this);
        pulseAlpha.addListener(this);
        pulseWidth.addListener(this);

        addAndMakeVisible(glowAlpha);
        addAndMakeVisible(glowWidth);
        addAndMakeVisible(pulseAlpha);
        addAndMakeVisible(pulseWidth);

        // captions above sliders
        prepCaption(glowAlphaCaption, "Glow Alpha");
        prepCaption(glowWidthCaption, "Glow Width (px)");
        prepCaption(pulseAlphaCaption, "Pulse Alpha");
        prepCaption(pulseWidthCaption, "Pulse Width (px)");

        // initialize + close
        addAndMakeVisible(btnResetDefaults);
        btnResetDefaults.setButtonText("Reset to Defaults");
        btnResetDefaults.addListener(this);

        addAndMakeVisible(btnClose);
        btnClose.setButtonText("Close");
        btnClose.addListener(this);

        refreshFromState();
    }

    void resized() override
    {
        auto a = getLocalBounds().reduced(12);

        auto toggleArea = a.removeFromTop(92);
        auto topRow = toggleArea.removeFromTop(28);
        showMasterBar.setBounds(topRow.removeFromLeft(getWidth() / 2 - 16));
        showSlotBars.setBounds(topRow);

        toggleArea.removeFromTop(8);
        auto secondRow = toggleArea.removeFromTop(28);
        showVisualizer.setBounds(secondRow.removeFromLeft(getWidth() / 2 - 16));
        secondRow.removeFromLeft(12);
        visualizerModeLabel.setBounds(secondRow.removeFromLeft(150));
        visualizerModeCombo.setBounds(secondRow.removeFromLeft(200).reduced(0, 4));

        auto sampleRateRow = a.removeFromTop(48);
        sampleRateLabel.setBounds(sampleRateRow.removeFromLeft(getWidth() / 2 - 16));
        sampleRateCombo.setBounds(sampleRateRow.removeFromLeft(180).reduced(0, 8));

        auto timingRow = a.removeFromTop(48);
        timingModeLabel.setBounds(timingRow.removeFromLeft(getWidth() / 2 - 16));
        timingModeCombo.setBounds(timingRow.removeFromLeft(220).reduced(0, 8));

        auto scaleRow = a.removeFromTop(48);
        slotScaleLabel.setBounds(scaleRow.removeFromLeft(getWidth() / 2 - 16));
        slotScaleCombo.setBounds(scaleRow.removeFromLeft(180).reduced(0, 8));

        a.removeFromTop(6);

        auto row1 = a.removeFromTop(210);
        {
            auto left = row1.removeFromLeft(getWidth() / 2 - 16);
            glowLabel.setBounds(left.removeFromTop(22));
            glowColourSel.setBounds(left);

            auto right = row1;
            pulseLabel.setBounds(right.removeFromTop(22));
            pulseColourSel.setBounds(right);
        }

        a.removeFromTop(8);

        auto row2 = a.removeFromTop(80);
        layoutSlider(row2.removeFromLeft(getWidth() / 2 - 16), glowAlpha);
        layoutSlider(row2, pulseAlpha);

        positionCaption(glowAlphaCaption, glowAlpha);
        positionCaption(pulseAlphaCaption, pulseAlpha);

        a.removeFromTop(8);

        auto row3 = a.removeFromTop(80);
        layoutSlider(row3.removeFromLeft(getWidth() / 2 - 16), glowWidth);
        layoutSlider(row3, pulseWidth);

        positionCaption(glowWidthCaption, glowWidth);
        positionCaption(pulseWidthCaption, pulseWidth);

        a.removeFromTop(8);

        auto bottom = a.removeFromBottom(40);
        btnResetDefaults.setBounds(bottom.removeFromLeft(180));
        btnClose.setBounds(bottom.removeFromRight(120));
    }

private:
    APVTS& apvts;

    juce::ToggleButton showMasterBar, showSlotBars, showVisualizer;
    juce::Label visualizerModeLabel;
    juce::ComboBox visualizerModeCombo;

    juce::Label sampleRateLabel;
    juce::ComboBox sampleRateCombo;
    juce::Label timingModeLabel;
    juce::ComboBox timingModeCombo;

    juce::Label slotScaleLabel;
    juce::ComboBox slotScaleCombo;

    juce::Label glowLabel, pulseLabel;
    juce::ColourSelector glowColourSel{ juce::ColourSelector::showColourAtTop
                                       | juce::ColourSelector::showSliders
                                       | juce::ColourSelector::showColourspace };
    juce::ColourSelector pulseColourSel{ juce::ColourSelector::showColourAtTop
                                       | juce::ColourSelector::showSliders
                                       | juce::ColourSelector::showColourspace };

    juce::Slider glowAlpha, glowWidth, pulseAlpha, pulseWidth;
    juce::TextButton btnResetDefaults, btnClose;

    juce::Label glowAlphaCaption, glowWidthCaption, pulseAlphaCaption, pulseWidthCaption;

    static constexpr int sliderLabelHeight = 18;
    static constexpr int sliderLabelGap = 2;
    static constexpr int sliderLabelTopPadding = 4;
    static constexpr int sliderLabelYOffset = 0;
    static constexpr int sliderHorizontalPadding = 8;
    static constexpr int sliderVerticalPadding = 8;

    std::function<void(float)> slotScaleChanged;
    std::array<int, 2>   sampleRateValues{ { 48000, 44100 } };
    std::array<int, 2>   timingModeValues{ { 0, 1 } };
    bool blockSampleRateUpdate = false;
    bool blockTimingModeUpdate = false;
    bool blockVisualizerModeUpdate = false;
    std::array<float, 6> slotScaleValues{ { 0.75f, 0.8f, 0.85f, 0.9f, 0.95f, 1.0f } };
    bool blockSlotScaleUpdate = false;

    // helpers
    static void setupSlider(juce::Slider& s, double mn, double mx, double inc, const juce::String& name)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        s.setRange(mn, mx, inc);
        s.setName(name);
    }
    static void layoutSlider(juce::Rectangle<int> area, juce::Slider& s)
    {
        auto sliderBounds = area.reduced(sliderHorizontalPadding, sliderVerticalPadding);
        const int top = area.getY() + sliderLabelTopPadding + sliderLabelHeight + sliderLabelGap;
        sliderBounds.setTop(std::min(top, sliderBounds.getBottom()));
        s.setBounds(sliderBounds);
    }
    void prepCaption(juce::Label& L, const juce::String& txt)
    {
        L.setText(txt, juce::dontSendNotification);
        L.setJustificationType(juce::Justification::centredLeft);
        L.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(L);
    }
    void positionCaption(juce::Label& caption, juce::Slider& slider)
    {
        caption.setBounds(slider.getX(),
                          slider.getY() - sliderLabelHeight - sliderLabelGap + sliderLabelYOffset,
                          slider.getWidth(),
                          sliderLabelHeight);
    }

    juce::Colour defaultGlowColour()  const { return juce::Colour::fromRGB(0x69, 0x94, 0xFC); }
    juce::Colour defaultPulseColour() const { return juce::Colour::fromRGB(0xD3, 0xCF, 0xE4); }

    void refreshFromState()
    {
        // toggles
        showMasterBar.setToggleState(Opt::getBool(apvts, "optShowMasterBar", true), juce::dontSendNotification);
        showSlotBars.setToggleState(Opt::getBool(apvts, "optShowSlotBars", true), juce::dontSendNotification);
        showVisualizer.setToggleState(Opt::getBool(apvts, "optShowVisualizer", false), juce::dontSendNotification);

        const bool edgeWalk = Opt::getBool(apvts, "optVisualizerEdgeWalk", true);
        blockVisualizerModeUpdate = true;
        visualizerModeCombo.setSelectedId(edgeWalk ? 1 : 2, juce::dontSendNotification);
        blockVisualizerModeUpdate = false;

        const int currentSampleRate = Opt::getInt(apvts, "optSampleRate", sampleRateValues.front());
        int sampleRateId = 1;
        for (int i = 0; i < (int)sampleRateValues.size(); ++i)
        {
            if (sampleRateValues[(size_t)i] == currentSampleRate)
            {
                sampleRateId = i + 1;
                break;
            }
        }

        blockSampleRateUpdate = true;
        sampleRateCombo.setSelectedId(sampleRateId, juce::dontSendNotification);
        blockSampleRateUpdate = false;

        const int timingMode = Opt::getInt(apvts, "optTimingMode", timingModeValues.front());
        int timingModeId = 1;
        for (int i = 0; i < (int)timingModeValues.size(); ++i)
        {
            if (timingModeValues[(size_t)i] == timingMode)
            {
                timingModeId = i + 1;
                break;
            }
        }

        blockTimingModeUpdate = true;
        timingModeCombo.setSelectedId(timingModeId, juce::dontSendNotification);
        blockTimingModeUpdate = false;

        const float currentScale = Opt::getFloat(apvts, "optSlotScale", 0.8f);
        int bestId = 1;
        float bestDiff = std::numeric_limits<float>::max();
        for (int i = 0; i < (int)slotScaleValues.size(); ++i)
        {
            const float diff = std::abs(currentScale - slotScaleValues[(size_t)i]);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                bestId = i + 1;
            }
        }

        blockSlotScaleUpdate = true;
        slotScaleCombo.setSelectedId(bestId, juce::dontSendNotification);
        blockSlotScaleUpdate = false;

        // colours
        glowColourSel.setCurrentColour(Opt::rgbParam(apvts, "optGlowColor", 0x6994FC, 1.0f));
        pulseColourSel.setCurrentColour(Opt::rgbParam(apvts, "optPulseColor", 0xD3CFE4, 1.0f));

        // sliders
        glowAlpha.setValue(Opt::getFloat(apvts, "optGlowAlpha", 0.431f), juce::dontSendNotification);
        glowWidth.setValue(Opt::getFloat(apvts, "optGlowWidth", 1.34f), juce::dontSendNotification);
        pulseAlpha.setValue(Opt::getFloat(apvts, "optPulseAlpha", 1.0f), juce::dontSendNotification);
        pulseWidth.setValue(Opt::getFloat(apvts, "optPulseWidth", 4.0f), juce::dontSendNotification);
    }

    // Button::Listener
    void buttonClicked(juce::Button* b) override
    {
        if (b == &showMasterBar)
            setBoolParam("optShowMasterBar", showMasterBar.getToggleState());
        else if (b == &showSlotBars)
            setBoolParam("optShowSlotBars", showSlotBars.getToggleState());
        else if (b == &showVisualizer)
            setBoolParam("optShowVisualizer", showVisualizer.getToggleState());
        else if (b == &btnResetDefaults)
            resetToDefaultOptions();
        else if (b == &btnClose)
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->closeButtonPressed();
    }

    // ChangeListener for colour selectors
    void changeListenerCallback(juce::ChangeBroadcaster* src) override
    {
        if (src == &glowColourSel)
        {
            const auto c = glowColourSel.getCurrentColour();
            setIntParam("optGlowColor", (int)((c.getRed() << 16) | (c.getGreen() << 8) | c.getBlue()));
            saveOptionsToDisk(apvts);
        }
        else if (src == &pulseColourSel)
        {
            const auto c = pulseColourSel.getCurrentColour();
            setIntParam("optPulseColor", (int)((c.getRed() << 16) | (c.getGreen() << 8) | c.getBlue()));
            saveOptionsToDisk(apvts);
        }
    }

    // Slider::Listener
    void sliderValueChanged(juce::Slider* s) override
    {
        if (s == &glowAlpha)  setFloatParam("optGlowAlpha", (float)glowAlpha.getValue());
        if (s == &glowWidth)  setFloatParam("optGlowWidth", (float)glowWidth.getValue());
        if (s == &pulseAlpha) setFloatParam("optPulseAlpha", (float)pulseAlpha.getValue());
        if (s == &pulseWidth) setFloatParam("optPulseWidth", (float)pulseWidth.getValue());
    }

    void handleVisualizerModeSelection()
    {
        if (blockVisualizerModeUpdate)
            return;

        const int id = visualizerModeCombo.getSelectedId();
        const bool edgeWalk = (id <= 0 || id == 1);
        setBoolParam("optVisualizerEdgeWalk", edgeWalk);
    }

    void handleSampleRateSelection()
    {
        if (blockSampleRateUpdate)
            return;

        const int id = sampleRateCombo.getSelectedId();
        if (id <= 0 || id > (int)sampleRateValues.size())
            return;

        const int value = sampleRateValues[(size_t)(id - 1)];
        setIntParam("optSampleRate", value);
    }

    void handleSlotScaleSelection()
    {
        if (blockSlotScaleUpdate)
            return;

        const int id = slotScaleCombo.getSelectedId();
        if (id <= 0 || id > (int)slotScaleValues.size())
            return;

        const float value = slotScaleValues[(size_t)(id - 1)];
        setFloatParam("optSlotScale", value);

        if (slotScaleChanged)
            slotScaleChanged(value);
    }

    void handleTimingModeSelection()
    {
        if (blockTimingModeUpdate)
            return;

        const int id = timingModeCombo.getSelectedId();
        if (id <= 0 || id > (int)timingModeValues.size())
            return;

        const int value = timingModeValues[(size_t)(id - 1)];
        setIntParam("optTimingMode", value);
    }

    void resetToDefaultOptions()
    {
        constexpr float kDefaultSlotScale = 0.80f;
        constexpr int   kDefaultGlowRGB = 0x6994FC;
        constexpr float kDefaultGlowAlpha = 0.431f;
        constexpr float kDefaultGlowWidth = 1.34f;
        constexpr int   kDefaultPulseRGB = 0xD3CFE4;
        constexpr float kDefaultPulseAlpha = 1.0f;
        constexpr float kDefaultPulseWidth = 4.0f;
        constexpr int   kDefaultSampleRate = 48000;
        constexpr int   kDefaultTimingMode = 0;

        setBoolParam("optShowMasterBar", true);
        setBoolParam("optShowSlotBars", true);
        setBoolParam("optShowVisualizer", false);
        setBoolParam("optVisualizerEdgeWalk", true);
        setIntParam("optSampleRate", kDefaultSampleRate);
        setIntParam("optTimingMode", kDefaultTimingMode);
        setFloatParam("optSlotScale", kDefaultSlotScale);
        setIntParam("optGlowColor", kDefaultGlowRGB);
        setFloatParam("optGlowAlpha", kDefaultGlowAlpha);
        setFloatParam("optGlowWidth", kDefaultGlowWidth);
        setIntParam("optPulseColor", kDefaultPulseRGB);
        setFloatParam("optPulseAlpha", kDefaultPulseAlpha);
        setFloatParam("optPulseWidth", kDefaultPulseWidth);

        refreshFromState();

        if (slotScaleChanged)
            slotScaleChanged(Opt::getFloat(apvts, "optSlotScale", kDefaultSlotScale));
    }

    // param setters
    void setBoolParam(const juce::String& id, bool v)
    {
        if (auto* b = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(id)))
        {
            b->beginChangeGesture(); *b = v; b->endChangeGesture();
            saveOptionsToDisk(apvts);
        }
    }
    void setIntParam(const juce::String& id, int v)
    {
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(id)))
        {
            ip->beginChangeGesture(); *ip = v; ip->endChangeGesture();
            saveOptionsToDisk(apvts);
        }
    }
    void setFloatParam(const juce::String& id, float v)
    {
        if (auto* fp = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(id)))
        {
            fp->beginChangeGesture(); *fp = v; fp->endChangeGesture();
            saveOptionsToDisk(apvts);
        }
    }
};


// ===== MIDI export helpers =====
namespace MidiExport
{
    static int igcd(int a, int b) { while (b != 0) { int t = a % b; a = b; b = t; } return a < 0 ? -a : a; }
    static int ilcm(int a, int b) { return (a == 0 || b == 0) ? 0 : (a / igcd(a, b)) * b; }

    // Continued fraction rational approximation
    static void approximateRational(double x, int maxDen, int& num, int& den)
    {
        int a0 = (int)std::floor(x);
        if (a0 > maxDen) { num = a0; den = 1; return; }
        int n0 = 1, d0 = 0;
        int n1 = a0, d1 = 1;
        double frac = x - (double)a0;
        while (frac > 1e-12 && d1 <= maxDen)
        {
            double inv = 1.0 / frac;
            int ai = (int)std::floor(inv);
            int n2 = n0 + ai * n1;
            int d2 = d0 + ai * d1;
            if (d2 > maxDen) break;
            n0 = n1; d0 = d1;
            n1 = n2; d1 = d2;
            frac = inv - (double)ai;
        }
        num = n1; den = d1;
    }

    struct SlotDef
    {
        int index = 0;      // 0-based slot index
        int note = 36;      // MIDI note
        int channel = 1;    // NEW: 1..16
        double rate = 1.0;  // hits per beat
        int count = 4;      // beats per shared cycle
        float gain = 0.8f;  // 0..1 for velocity
    };
}

// ===== Editor =====
SlotMachineAudioProcessorEditor::SlotMachineAudioProcessorEditor(SlotMachineAudioProcessor& p, APVTS& state)
    : juce::AudioProcessorEditor(&p), processor(p), apvts(state), tooltipWindow(this, 600)   // <— add this here
{
    setWantsKeyboardFocus(true);

    logoImage = juce::ImageCache::getFromMemory(BinaryData::SM5_png, BinaryData::SM5_pngSize);

    slotScale = juce::jlimit(0.75f, 1.0f, Opt::getFloat(apvts, "optSlotScale", 0.8f));

    constexpr int slotColumns = 4;
    const int slotRows = juce::jmax(1, (kNumSlots + slotColumns - 1) / slotColumns);
    const int slotRowHeight = scaleDimension(220);
    const int chromeHeight = scaleDimension(160) + kMasterControlsYOffset; // top/bottom padding + master row + tabs space
    setSize(1280, chromeHeight + slotRows * slotRowHeight);

    if (processor.consumeInitialiseOnFirstEditor())
        processor.initialiseStateForFirstEditor();

    updateStandaloneWindowTitle();

    // Master row
    addAndMakeVisible(masterLabel);
    masterLabel.setJustificationType(juce::Justification::bottomLeft);
    masterLabel.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    masterLabel.setFont(createBoldFont(18.0f));
    masterLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    masterLabel.setTooltip("Tap tempo");
    masterLabel.addMouseListener(this, false);

    addAndMakeVisible(masterBPM);
    masterBPM.setSliderStyle(juce::Slider::LinearHorizontal);
    masterBPM.setRange(10.0, 1000.0, 0.01);
    masterBPM.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, 22);
    masterBPM.setName("Master BPM");
    masterBPMA = std::make_unique<APVTS::SliderAttachment>(apvts, "masterBPM", masterBPM);

    masterRunA = std::make_unique<APVTS::ButtonAttachment>(apvts, "masterRun", startToggle);

    auto beautify = [](juce::TextButton& b)
        {
            b.setClickingTogglesState(false);
            b.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            b.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            b.setColour(juce::TextButton::buttonOnColourId, juce::Colours::grey);
        };

    addAndMakeVisible(btnStart);       beautify(btnStart);       btnStart.addListener(this);
    btnStart.addShortcut(juce::KeyPress(juce::KeyPress::spaceKey));
    addAndMakeVisible(btnSave);        beautify(btnSave);        btnSave.addListener(this);
    addAndMakeVisible(btnLoad);        beautify(btnLoad);        btnLoad.addListener(this);
    addAndMakeVisible(btnResetLoop);   beautify(btnResetLoop);   btnResetLoop.addListener(this);
    addAndMakeVisible(btnReset);       beautify(btnReset);       btnReset.addListener(this);
    addAndMakeVisible(btnInitialize);  beautify(btnInitialize);  btnInitialize.addListener(this);
    addAndMakeVisible(btnOptions);     beautify(btnOptions);     btnOptions.addListener(this);
    addAndMakeVisible(btnExportMidi);   beautify(btnExportMidi);   btnExportMidi.addListener(this);
    addAndMakeVisible(btnExportAudio);  beautify(btnExportAudio);  btnExportAudio.addListener(this);
    addAndMakeVisible(btnVisualizer);   beautify(btnVisualizer);   btnVisualizer.addListener(this);
    addAndMakeVisible(btnUserManual);   beautify(btnUserManual);   btnUserManual.addListener(this);
    addAndMakeVisible(btnAbout);        beautify(btnAbout);        btnAbout.addListener(this);

    addAndMakeVisible(patternTabs);
    patternTabs.onTabSelected([this](int index)
        {
            if (fileDialogActive)
            {
                patternTabs.setCurrentIndex(currentPatternIndex);
                return;
            }

            if (index == currentPatternIndex)
                return;

            applyPattern(index, true, true, true);
        });

    patternTabs.onTabBarRightClick([this](const juce::MouseEvent& e)
        {
            handlePatternContextMenu(e);
        });

    patternTabs.onTabReordered([this](int fromIndex, int toIndex)
        {
            reorderPatterns(fromIndex, toIndex);
        });

    addAndMakeVisible(patternWarningLabel);
    patternWarningLabel.setColour(juce::Label::textColourId, juce::Colours::orange.withAlpha(0.85f));
    patternWarningLabel.setJustificationType(juce::Justification::centredRight);
    patternWarningLabel.setVisible(false);
    patternWarningLabel.setFont(createBoldFont(13.0f));

    // Slots
    const juce::Image muteOffImage = juce::ImageCache::getFromMemory(BinaryData::MuteOFF_png, BinaryData::MuteOFF_pngSize);
    const juce::Image muteOnImage  = juce::ImageCache::getFromMemory(BinaryData::MuteON_png,  BinaryData::MuteON_pngSize);
    const juce::Image soloOffImage = juce::ImageCache::getFromMemory(BinaryData::SoloOFF_png, BinaryData::SoloOFF_pngSize);
    const juce::Image soloOnImage  = juce::ImageCache::getFromMemory(BinaryData::SoloON_png,  BinaryData::SoloON_pngSize);

    const auto configureToggleImageButton = [](juce::ImageButton& button,
                                               const juce::Image& offImage,
                                               const juce::Image& onImage)
    {
        auto updateImages = [offImage, onImage, &button]()
        {
            const auto& source = button.getToggleState() ? onImage : offImage;

            button.setImages(false, true, true,
                source, 1.0f, juce::Colours::transparentBlack,
                source, 1.0f, juce::Colours::transparentBlack,
                source, 1.0f, juce::Colours::transparentBlack);
        };

        button.setClickingTogglesState(true);
        updateImages();
        button.onStateChange = updateImages;
    };

    for (int i = 0; i < kNumSlots; ++i)
    {
        auto ui = std::make_unique<SlotUI>();
        const int slotIndex = i;
        const int idx = slotIndex + 1;

        ui->group.setText("SLOT " + juce::String(idx));
        addAndMakeVisible(ui->group);

        ui->group.setInterceptsMouseClicks(true, true);
        ui->group.addMouseListener(this, true);   // listen to the group + its children


        addAndMakeVisible(ui->fileBtn);
        addAndMakeVisible(ui->clearBtn);                // NEW
        ui->clearBtn.setTooltip("Clear sample");        // NEW
        ui->clearBtn.addListener(this);                 // NEW

        ui->clearBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        ui->clearBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.85f));
        ui->clearBtn.setConnectedEdges(juce::Button::ConnectedOnLeft); // makes it look attached to Load

        addAndMakeVisible(ui->fileLabel);
        ui->fileLabel.setText("No file", juce::dontSendNotification);
        ui->fileLabel.setJustificationType(juce::Justification::centredLeft);
        ui->fileBtn.addListener(this);
        ui->fileBtn.onFileDropped = [this, i](const juce::File& file)
        {
            handleSlotFileSelection(i, file);
        };

        addAndMakeVisible(ui->midiChannel);
        addAndMakeVisible(ui->muteBtn);
        addAndMakeVisible(ui->soloBtn);
        addAndMakeVisible(ui->muteLabel);
        addAndMakeVisible(ui->soloLabel);

        ui->midiChannel.setName("MidiChannel" + juce::String(idx));
        ui->midiChannel.setJustificationType(juce::Justification::centred);
        ui->midiChannel.setTooltip("Select the MIDI channel used when this slot triggers events");
        ui->midiChannel.setTextWhenNothingSelected("Ch " + juce::String(idx));
        for (int ch = 1; ch <= 16; ++ch)
            ui->midiChannel.addItem("Ch " + juce::String(ch), ch);

        ui->muteBtn.setName("MuteButton" + juce::String(idx));
        configureToggleImageButton(ui->muteBtn, muteOffImage, muteOnImage);

        ui->soloBtn.setName("SoloButton" + juce::String(idx));
        configureToggleImageButton(ui->soloBtn, soloOffImage, soloOnImage);

        ui->muteLabel.setText("Mute", juce::dontSendNotification);
        ui->muteLabel.setJustificationType(juce::Justification::centred);
        ui->muteLabel.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
        ui->muteLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        ui->muteLabel.target = &ui->muteBtn;

        ui->soloLabel.setText("Solo", juce::dontSendNotification);
        ui->soloLabel.setJustificationType(juce::Justification::centred);
        ui->soloLabel.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
        ui->soloLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        ui->soloLabel.target = &ui->soloBtn;

        ui->muteBtn.addListener(this);
        ui->soloBtn.addListener(this);

        addAndMakeVisible(ui->count);
        addAndMakeVisible(ui->rate);
        addAndMakeVisible(ui->gain);
        addAndMakeVisible(ui->decay);

        setupKnob(ui->count, 1.0, (double)SlotMachineAudioProcessorEditor::kMaxBeatsPerSlot, 1.0, "Beats/Cycle (Count)");
        ui->count.setNumDecimalPlacesToDisplay(0);
        ui->count.setTooltip("Number of beats in one shared cycle.");
        setupKnob(ui->rate, 0.0625, 4.00, 0.0001, "Rate", 4);
        setupKnob(ui->gain, 0.0, 100.0, 0.1, "Gain");
        setupKnob(ui->decay, 1.0, 100.0, 0.1, "Decay (ms)");

        ui->rate.onValueChange = [this, slotIndex]()
        {
            if (auto* slot = slots[(size_t)slotIndex].get())
                handleSlotRateChanged(slotIndex, *slot);
        };
        ui->count.onValueChange = [this, slotIndex]()
        {
            if (auto* slot = slots[(size_t)slotIndex].get())
            {
                slot->beatsQuickPickExpanded = slot->count.getValue() > kBeatsQuickPickDefaultMax;
                handleSlotCountChanged(slotIndex, *slot);
            }
        };

        ui->count.addMouseListener(this, true);

        ui->muteA = std::make_unique<APVTS::ButtonAttachment>(apvts, "slot" + juce::String(idx) + "_Mute", ui->muteBtn);
        ui->soloA = std::make_unique<APVTS::ButtonAttachment>(apvts, "slot" + juce::String(idx) + "_Solo", ui->soloBtn);

        if (ui->muteBtn.onStateChange)
            ui->muteBtn.onStateChange();

        if (ui->soloBtn.onStateChange)
            ui->soloBtn.onStateChange();

        ui->countA = std::make_unique<APVTS::SliderAttachment>(apvts, "slot" + juce::String(idx) + "_Count", ui->count);
        ui->rateA = std::make_unique<APVTS::SliderAttachment>(apvts, "slot" + juce::String(idx) + "_Rate", ui->rate);
        ui->gainA = std::make_unique<APVTS::SliderAttachment>(apvts, "slot" + juce::String(idx) + "_Gain", ui->gain);
        ui->decayA = std::make_unique<APVTS::SliderAttachment>(apvts, "slot" + juce::String(idx) + "_Decay", ui->decay);
        ui->midiChannelA = std::make_unique<APVTS::ComboBoxAttachment>(apvts, "slot" + juce::String(idx) + "_MidiChannel", ui->midiChannel);

        ui->hasFile = processor.slotHasSample(i);
        auto existing = processor.getSlotFilePath(i);
        if (existing.isNotEmpty())
            ui->fileLabel.setText(juce::File(existing).getFileName(), juce::dontSendNotification);

        slots[(size_t)i] = std::move(ui);

        if (auto* slotPtr = slots[(size_t)i].get())
            initialiseSlotTimingPair(i, *slotPtr);
    }

    initialisePatterns();

    startTimerHz(60);
    lastPhase = (float)processor.getMasterPhase();

    lastStartToggleState = startToggle.getToggleState();
    cachedStartGlowColour = Opt::rgbParam(apvts, "optGlowColor", 0x6994FC, 1.0f);
    cachedStartPulseColour = Opt::rgbParam(apvts, "optPulseColor", 0xD3CFE4, 1.0f);
    cachedStartGlowAlpha = Opt::getFloat(apvts, "optGlowAlpha", 0.431f);
    cachedStartGlowWidth = Opt::getFloat(apvts, "optGlowWidth", 1.34f);
    updateStartButtonVisuals(lastStartToggleState, cachedStartGlowColour,
        cachedStartPulseColour, cachedStartGlowAlpha, cachedStartGlowWidth);
    updateSliderKnobColours(cachedStartPulseColour);

    resized();
    repaint();

    // Standalone fallback: load Options from disk if host didn't restore
    const float slotScaleBeforeOptionsLoad = slotScale;
    loadOptionsFromDiskIfNoHostState(apvts);

    const float startupSlotScale = Opt::getFloat(apvts, "optSlotScale", slotScaleBeforeOptionsLoad);
    if (std::abs(startupSlotScale - slotScaleBeforeOptionsLoad) < 0.0001f)
    {
        refreshSizeForSlotScale();
        resized();
        repaint();
    }
    else
    {
        applySlotScale(startupSlotScale);
    }

    lastShowVisualizer = Opt::getBool(apvts, "optShowVisualizer", false);
    if (lastShowVisualizer)
        openVisualizerWindow();

    lastTimingMode = Opt::getInt(apvts, "optTimingMode", 0);
}

SlotMachineAudioProcessorEditor::~SlotMachineAudioProcessorEditor()
{
    closeVisualizerWindow();
}

void SlotMachineAudioProcessorEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    updateStandaloneWindowTitle();
}

void SlotMachineAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    juce::AudioProcessorEditor::mouseDown(e);

    auto* eventComponent = e.eventComponent;
    if (eventComponent == nullptr)
        return;

    for (int i = 0; i < kNumSlots; ++i)
    {
        if (auto* slot = slots[(size_t)i].get())
        {
            auto& beatsSlider = slot->count;
            const bool hitBeatsControl = (eventComponent == &beatsSlider) || beatsSlider.isParentOf(eventComponent);

            if (hitBeatsControl && e.mods.isPopupMenu())
            {
                const int currentValue = (int)std::round(beatsSlider.getValue());

                BeatsQuickPickGrid::Options opts;
                opts.maxBeat = slot->beatsQuickPickExpanded ? kMaxBeatsPerSlot : kBeatsQuickPickDefaultMax;
                if (currentValue > kBeatsQuickPickDefaultMax)
                    opts.maxBeat = kMaxBeatsPerSlot;

                slot->beatsQuickPickExpanded = opts.maxBeat > kBeatsQuickPickDefaultMax;

                auto pickHandler = [slot](int picked)
                {
                    slot->beatsQuickPickExpanded = picked > kBeatsQuickPickDefaultMax;
                    slot->count.setValue(picked, juce::sendNotificationSync);
                };

                auto* grid = new BeatsQuickPickGrid(opts, std::move(pickHandler), currentValue);
                slot->beatsQuickPickExpanded = grid->isExpanded();

                const auto screenPos = e.getScreenPosition().roundToInt();
                auto calloutBounds = juce::Rectangle<int>(screenPos.x, screenPos.y, 1, 1);
                juce::CallOutBox::launchAsynchronously(*grid, calloutBounds, nullptr);
                return;
            }
        }
    }
}

void SlotMachineAudioProcessorEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    auto* eventComponent = e.eventComponent;
    if (eventComponent == nullptr)
    {
        juce::AudioProcessorEditor::mouseWheelMove(e, wheel);
        return;
    }

    for (int i = 0; i < kNumSlots; ++i)
    {
        if (auto* slot = slots[(size_t)i].get())
        {
            auto& beatsSlider = slot->count;
            const bool hitBeatsControl = (eventComponent == &beatsSlider) || beatsSlider.isParentOf(eventComponent);

            if (hitBeatsControl)
            {
                if (wheel.deltaY == 0.0f)
                    return;

                const bool accelerated = e.mods.isCtrlDown() || e.mods.isCommandDown();
                const int step = accelerated ? 4 : 1;

                int value = (int)std::round(beatsSlider.getValue());
                if (wheel.deltaY > 0.0f)
                    value += step;
                else if (wheel.deltaY < 0.0f)
                    value -= step;

                const int limit = slot->beatsQuickPickExpanded ? kMaxBeatsPerSlot : kBeatsQuickPickDefaultMax;
                value = juce::jlimit(1, limit, value);

                if (value != (int)std::round(beatsSlider.getValue()))
                    beatsSlider.setValue(value, juce::sendNotificationSync);

                slot->beatsQuickPickExpanded = value > kBeatsQuickPickDefaultMax;
                return;
            }
        }
    }

    juce::AudioProcessorEditor::mouseWheelMove(e, wheel);
}

void SlotMachineAudioProcessorEditor::updateStandaloneWindowTitle()
{
#if JUCE_STANDALONE_APPLICATION
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(kStandaloneWindowTitle);
#endif
}

void SlotMachineAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    if (logoImage.isValid() && !logoBounds.isEmpty())
        g.drawImage(logoImage,
                    (float)logoBounds.getX(),
                    (float)logoBounds.getY(),
                    (float)logoBounds.getWidth(),
                    (float)logoBounds.getHeight(),
                    0.0f,
                    0.0f,
                    (float)logoImage.getWidth(),
                    (float)logoImage.getHeight());

    // Options
    const bool showMasterBar = Opt::getBool(apvts, "optShowMasterBar", true);
    const bool showSlotBars = Opt::getBool(apvts, "optShowSlotBars", true);

    const float glowAlpha = Opt::getFloat(apvts, "optGlowAlpha", 0.431f);
    const float glowWidthPx = Opt::getFloat(apvts, "optGlowWidth", 1.34f);
    const float pulseAlpha = Opt::getFloat(apvts, "optPulseAlpha", 1.0f);
    const float pulseWidthPx = Opt::getFloat(apvts, "optPulseWidth", 4.0f);

    const juce::Colour glowColour = Opt::rgbParam(apvts, "optGlowColor", 0x6994FC, glowAlpha);
    const juce::Colour pulseColour = Opt::rgbParam(apvts, "optPulseColor", 0xD3CFE4, pulseAlpha);

    const auto barBack = juce::Colours::white.withAlpha(0.18f);
    const auto barFill = pulseColour.withAlpha(0.92f);

    // Master progress bar
    if (showMasterBar)
    {
        g.setColour(barBack);
        g.fillRoundedRectangle(masterBarBounds.toFloat(), 3.0f);

        const float w = masterBarBounds.getWidth() * juce::jlimit(0.0f, 1.0f, masterPhase);
        if (w > 1.0f)
        {
            auto filled = juce::Rectangle<float>((float)masterBarBounds.getX(),
                (float)masterBarBounds.getY(),
                w,
                (float)masterBarBounds.getHeight());
            g.setColour(barFill);
            g.fillRoundedRectangle(filled, 3.0f);
        }

        // Flash overlay on cycle wrap, using the selected pulse colour/alpha
        if (cycleFlash > 0.001f)
        {
            // use the same 'pulseColour' and its alpha you already read from Options
            const float flashA = juce::jlimit(0.0f, 1.0f, cycleFlash);
            auto flashCol = pulseColour.withAlpha(pulseColour.getFloatAlpha() * flashA);

            // subtle full-bar glow
            g.setColour(flashCol);
            g.fillRoundedRectangle(masterBarBounds.toFloat(), 3.0f);

            // crisp highlight line at bar start (downbeat tick)
            g.setColour(juce::Colours::white.withAlpha(0.25f * flashA));
            auto tick = masterBarBounds.toFloat().removeFromLeft(3.0f);
            g.fillRoundedRectangle(tick, 2.0f);
        }
    }

    // Slots
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui) continue;

        const auto boundsF = ui->group.getBounds().toFloat();

        // 1) Glow + pulse frame
        {
            auto frame = boundsF.reduced(1.5f, 1.5f);
            const bool  selected = ui->hasFile;
            const float pulse = ui->glow;
            const int   layers = 5;
            const float baseThick = glowWidthPx;

            const juce::Colour selColour = selected ? glowColour : glowColour.withAlpha(0.0f);
            drawNeonFrame(g, frame, 10.0f,
                selColour, layers, baseThick,
                pulseColour, pulseWidthPx, pulse);
        }

        // 2) Per-slot progress bar
        if (showSlotBars)
        {
            const float barH = 8.0f;
            auto inner = boundsF.reduced(8.0f, 8.0f);
            auto bar = juce::Rectangle<float>(inner.getX(), inner.getBottom() - barH,
                inner.getWidth(), barH);

            g.setColour(barBack);
            g.fillRoundedRectangle(bar, 3.0f);

            if (ui->hasFile)
            {
                const float w = bar.getWidth() * juce::jlimit(0.0f, 1.0f, ui->phase);
                if (w > 1.0f)
                {
                    g.setColour(pulseColour);
                    g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX(), bar.getY(), w, barH), 3.0f);
                }
            }
        }

        // 3) Knob labels
        auto drawKnobLabel = [&g](juce::Slider& slider, const juce::String& text)
            {
                auto layout = slider.getLookAndFeel().getSliderLayout(slider);
                auto knobBounds = layout.sliderBounds.toFloat();

                if (!knobBounds.isEmpty())
                    knobBounds += slider.getPosition().toFloat();
                else
                    knobBounds = slider.getBounds().toFloat();

                const auto centre = knobBounds.getCentre();
                const auto size = juce::jmin(knobBounds.getWidth(), knobBounds.getHeight());
                auto square = juce::Rectangle<float>(size, size).withCentre(centre);
                auto labelArea = square.reduced(size * 0.28f, size * 0.28f);

                g.setFont(createBoldFont(13.0f));
                const bool enabled = slider.isEnabled();
                const auto textColour = enabled ? juce::Colours::white.withAlpha(0.90f)
                                                : juce::Colours::lightgrey.withAlpha(0.65f);
                const auto shadowColour = enabled ? juce::Colours::black.withAlpha(0.55f)
                                                  : juce::Colours::black.withAlpha(0.25f);
                g.setColour(shadowColour);
                g.drawFittedText(text, labelArea.translated(0, 1).toNearestInt(), juce::Justification::centred, 1);

                g.setColour(textColour);
                g.drawFittedText(text, labelArea.toNearestInt(), juce::Justification::centred, 1);
            };

        drawKnobLabel(ui->count, "COUNT");
        drawKnobLabel(ui->rate, "RATE");
        drawKnobLabel(ui->gain, "VOL");
        drawKnobLabel(ui->decay, "DECAY");
    }
}

void SlotMachineAudioProcessorEditor::resetUiToDefaultStateForStandalone()
{
    doResetAll(false);
}

void SlotMachineAudioProcessorEditor::resized()
{
    auto slotScaled = [this](int value) { return scaleDimension(value); };

    const int margin = 12;
    auto bounds = getLocalBounds();
    auto area = bounds.reduced(margin);

    if (logoImage.isValid())
    {
        const int imageWidth = logoImage.getWidth();
        const int imageHeight = logoImage.getHeight();

        if (imageWidth > 0 && imageHeight > 0)
        {
            const float logoScaleFactor = 1.3f;
            const float maxWidth = 160.0f * logoScaleFactor;
            const float maxHeight = 32.0f * logoScaleFactor;
            const float baseScale = juce::jmin(maxWidth / (float)imageWidth,
                                               maxHeight / (float)imageHeight,
                                               1.0f);
            const float scale = juce::jmax(0.0f, baseScale * kBannerScaleMultiplier);
            const int width = juce::jmax(1, juce::roundToInt((float)imageWidth * scale));
            const int height = juce::jmax(1, juce::roundToInt((float)imageHeight * scale));

            logoBounds = { bounds.getX() + margin + 15, bounds.getY() + 4 + 17, width, height };
        }
        else
        {
            logoBounds = {};
        }
    }
    else
    {
        logoBounds = {};
    }

    // Master row
    {
        const int sliderHeight = 32;
        const int sliderGap = 12;
        const int buttonHeight = 36;
        const int buttonInsetY = 8;
        const int bottomMargin = 6 + kMasterControlsYOffset;
        const int buttonRowGap = 4;
        const int buttonRowsHeight = buttonInsetY * 2 + buttonHeight * 2 + buttonRowGap;
        const int masterHeight = juce::jmax(sliderHeight, buttonRowsHeight) + bottomMargin;

        auto top = area.removeFromTop(masterHeight);

        auto labelArea = top.removeFromLeft(170);
        auto sliderArea = top.removeFromLeft(420);

        auto buttonArea = top.reduced(10, buttonInsetY);
        const int numButtons = 10; // Start/Save/Load/Reset Loop/Reset UI/Initialize/Options/Export MIDI/Export Audio/Visualizer (User Manual and About below)
        const int bw = buttonArea.getWidth() / numButtons;
        const int bh = buttonHeight;
        const int firstRowY = buttonArea.getY();
        const int secondRowY = firstRowY + bh + buttonRowGap;
        const int buttonBottom = secondRowY + bh;

        auto labelBounds = labelArea.reduced(8, 0);

        auto sliderBounds = sliderArea.withTrimmedRight(10).withHeight(sliderHeight);
        sliderBounds.setBottom(buttonBottom - sliderGap + kMasterControlsYOffset);
        sliderBounds.setLeft(labelBounds.getRight());
        sliderBounds.translate(-35, 0);
        sliderBounds.setWidth(juce::jmax(0, sliderBounds.getWidth() - 55));
        masterBPM.setBounds(sliderBounds);

        const auto textBoxBottom = sliderBounds.getY() + masterBPM.getTextBoxHeight();
        const auto labelHeight = (int)std::ceil(masterLabel.getFont().getHeight());
        const auto labelOffset = 20;

        labelBounds.setHeight(labelHeight);
        labelBounds.setBottom(textBoxBottom + labelOffset);
        labelBounds.translate(0, kMasterLabelExtraYOffset);
        masterLabel.setBounds(labelBounds);

        const int barH = 8;
        const int barLeft = buttonArea.getX();
        const int userManualLeft = buttonArea.getX() + 7 * bw;
        const int barRight = userManualLeft - 20; // keep a 20px gap before the User Manual button
        const int barWidth = juce::jmax(0, barRight - barLeft);
        masterBarBounds = juce::Rectangle<int>(barLeft,
                                               buttonBottom - barH - 10,
                                               barWidth,
                                               barH);

        auto firstRowBounds = [&](int index)
        {
            return juce::Rectangle<int>(buttonArea.getX() + index * bw, firstRowY, bw, bh);
        };

        auto secondRowBounds = [&](int index)
        {
            return juce::Rectangle<int>(buttonArea.getX() + index * bw, secondRowY, bw, bh);
        };

        btnStart.setBounds(firstRowBounds(0));
        btnSave.setBounds(firstRowBounds(1));
        btnLoad.setBounds(firstRowBounds(2));
        btnResetLoop.setBounds(firstRowBounds(3));
        btnReset.setBounds(firstRowBounds(4));
        btnInitialize.setBounds(firstRowBounds(5));
        btnOptions.setBounds(firstRowBounds(6));
        btnExportMidi.setBounds(firstRowBounds(7));
        btnExportAudio.setBounds(firstRowBounds(8));
        btnVisualizer.setBounds(firstRowBounds(9));
        btnUserManual.setBounds(secondRowBounds(7));
        btnAbout.setBounds(secondRowBounds(8));
    }

    const int tabsLift = 73;

    auto tabsRow = area.removeFromTop(36);
    tabsRow.translate(0, -tabsLift);
    auto warningArea = tabsRow.removeFromRight(220).reduced(10, 4);
    patternWarningLabel.setBounds(warningArea);
    patternTabs.setBounds(tabsRow.reduced(0, 4));

    area.translate(0, -tabsLift);
    area.setBottom(bounds.getBottom() - margin);

    // Grid layout (4 columns by as many rows as needed)
    const int columns = 4;
    const int rows = juce::jmax(1, (kNumSlots + columns - 1) / columns);
    const int gridX = area.getX(), gridY = area.getY();
    const int gridW = area.getWidth(), gridH = area.getHeight();
    const int cellW = gridW / columns, cellH = gridH / rows;
    const int pad = slotScaled(6), innerPad = slotScaled(12);

    for (int i = 0; i < kNumSlots; ++i)
    {
        if (!slots[(size_t)i]) continue;

        const int row = i / columns, col = i % columns;
        const int x = gridX + col * cellW + pad;
        const int y = gridY + row * cellH + pad;
        const int w = cellW - 2 * pad;
        const int h = cellH - 2 * pad;

        auto& ui = *slots[(size_t)i];
        ui.group.setBounds(x, y, w, h);

        const int raiseAmount = juce::jmax(1, scaleDimension(4));
        int contentYOffset = 0;

        // JUCE 7 exposes the group title label, but older builds do not.  Use the
        // accessor when it's available and fall back to pushing the slot contents
        // down slightly when it isn't so the visual spacing still improves.
        if (auto* titleLabel = getSlotTitleLabelIfAvailable(ui.group))
        {
            auto labelBounds = titleLabel->getBounds();
            labelBounds.translate(0, ui.titleLabelRaiseOffset);
            labelBounds.translate(0, -raiseAmount);
            titleLabel->setBounds(labelBounds);

            ui.titleLabelRaiseOffset = raiseAmount;
        }
        else
        {
            ui.titleLabelRaiseOffset = 0;
            contentYOffset = raiseAmount;
        }

        const int ix = x + innerPad;
        const int iy = y + innerPad + contentYOffset;
        const int iw = w - 2 * innerPad;

        const int fileRowH = slotScaled(28);
        const int loadW = scaleDimension(110);  // your existing Load width
        const int clearW = scaleDimension(24);   // tiny X button
        const int gap = slotScaled(4);

        ui.fileBtn.setBounds(ix, iy, loadW, fileRowH);
        ui.clearBtn.setBounds(ix + loadW + gap, iy, clearW, fileRowH);

        // filename label fills the rest
        const int labelX = ix + loadW + gap + clearW + gap;
        const int labelW = juce::jmax(0, iw - (labelX - ix));
        ui.fileLabel.setBounds(labelX, iy, labelW, fileRowH);


        const int knobsY = iy + fileRowH + slotScaled(4);
        const int knobsH = slotScaled(112);
        const int quarterW = iw / 4;

        const int knobW = juce::jmax(8, quarterW - slotScaled(8));
        ui.count.setBounds(ix, knobsY, knobW, knobsH);
        ui.rate.setBounds(ix + quarterW, knobsY, knobW, knobsH);
        ui.gain.setBounds(ix + 2 * quarterW, knobsY, knobW, knobsH);
        ui.decay.setBounds(ix + 3 * quarterW, knobsY, knobW, knobsH);

        const int buttonW = scaleDimension(60);
        const int buttonH = slotScaled(22);
        const int labelHeight = slotScaled(16);
        const int labelGapY = slotScaled(2);
        const int midiComboW = scaleDimensionWithMax(80, 0.95f);
        const int midiComboH = scaleDimensionWithMax(22, 0.95f);
        const int controlBlockHeight = juce::jmax(midiComboH, buttonH + labelGapY + labelHeight);

        const int knobsBottom = knobsY + knobsH;
        const int progressInset = juce::roundToInt(8.0f * slotScale);
        const int progressHeight = juce::roundToInt(8.0f * slotScale);
        const int progressTop = ui.group.getBottom() - progressInset - progressHeight;
        const int availableSpace = juce::jmax(0, progressTop - knobsBottom);
        int togglesY = knobsBottom + juce::jmax(0, (availableSpace - controlBlockHeight) / 2);

        const int absoluteMaxToggleY = progressTop - controlBlockHeight;
        const int minToggleY = knobsBottom;

        if (absoluteMaxToggleY >= minToggleY)
        {
            const int safetyMargin = juce::roundToInt(4.0f * slotScale);
            const int usableMaxToggleY = juce::jmax(minToggleY, absoluteMaxToggleY - safetyMargin);
            togglesY = juce::jlimit(minToggleY, usableMaxToggleY, togglesY);
        }
        else
        {
            togglesY = absoluteMaxToggleY;
        }

        const int countCentreX = ui.count.getBounds().getCentreX();
        const int rateCentreX = ui.rate.getBounds().getCentreX();
        const int gainCentreX = ui.gain.getBounds().getCentreX();
        const int decayCentreX = ui.decay.getBounds().getCentreX();

        const int midiY = togglesY + (controlBlockHeight - midiComboH) / 2;
        ui.midiChannel.setBounds(countCentreX - midiComboW / 2, midiY, midiComboW, midiComboH);

        const int buttonY = togglesY;
        const int labelY = buttonY + buttonH + labelGapY;
        ui.muteBtn.setBounds(gainCentreX - buttonW / 2, buttonY, buttonW, buttonH);
        ui.muteLabel.setBounds(gainCentreX - buttonW / 2, labelY, buttonW, labelHeight);

        ui.soloBtn.setBounds(decayCentreX - buttonW / 2, buttonY, buttonW, buttonH);
        ui.soloLabel.setBounds(decayCentreX - buttonW / 2, labelY, buttonW, labelHeight);
    }
}

int SlotMachineAudioProcessorEditor::scaleDimension(int base) const
{
    if (base == 0)
        return 0;

    const float scaled = (float)base * slotScale;
    if (base > 0)
        return juce::jmax(1, juce::roundToInt(scaled));

    return juce::jmin(-1, juce::roundToInt(scaled));
}

int SlotMachineAudioProcessorEditor::scaleDimensionWithMax(int base, float maxScale) const
{
    if (base == 0)
        return 0;

    const float appliedScale = juce::jmax(0.0f, juce::jmin(slotScale, maxScale));
    const float scaled = (float)base * appliedScale;
    if (base > 0)
        return juce::jmax(1, juce::roundToInt(scaled));

    return juce::jmin(-1, juce::roundToInt(scaled));
}

void SlotMachineAudioProcessorEditor::refreshSizeForSlotScale()
{
    constexpr int slotColumns = 4;
    const int slotRows = juce::jmax(1, (kNumSlots + slotColumns - 1) / slotColumns);
    const int slotRowHeight = scaleDimension(220);
    const int chromeHeight = 200 + kMasterControlsYOffset;
    const int newHeight = chromeHeight + slotRows * slotRowHeight;
    const int currentWidth = juce::jmax(1, getWidth());
    setSize(currentWidth, newHeight);
}

void SlotMachineAudioProcessorEditor::applySlotScale(float newScale)
{
    const float clamped = juce::jlimit(0.75f, 1.0f, newScale);
    if (std::abs(clamped - slotScale) < 0.0001f)
        return;

    slotScale = clamped;
    refreshSizeForSlotScale();
    resized();
    repaint();
}

void SlotMachineAudioProcessorEditor::handleSlotFileSelection(int slotIndex, const juce::File& file)
{
    if (!file.existsAsFile())
        return;

    const bool loaded = processor.loadSampleForSlot(slotIndex, file, startToggle.getToggleState());

    juce::Array<int> failed;
    if (!loaded)
        failed.add(slotIndex);

    refreshSlotFileLabels(failed);
    showPatternWarning(failed);
    saveCurrentPattern();
    repaint();
}

juce::String SlotMachineAudioProcessorEditor::defaultPatternNameForIndex(int index) const
{
    juce::String result;
    int value = index;

    do
    {
        const int remainder = value % 26;
        result = juce::String::charToString((juce::juce_wchar)('A' + remainder)) + result;
        value = value / 26 - 1;
    }
    while (value >= 0);

    return result;
}

void SlotMachineAudioProcessorEditor::initialisePatterns()
{
    patternsTree = processor.getPatternsTree();
    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    currentPatternIndex = juce::jlimit(0, count - 1, processor.getCurrentPatternIndex());
    processor.setCurrentPatternIndex(currentPatternIndex);

    refreshPatternTabs();
    applyPattern(currentPatternIndex, true, false);
}

void SlotMachineAudioProcessorEditor::saveCurrentPattern()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    currentPatternIndex = juce::jlimit(0, count - 1, currentPatternIndex);
    processor.storeCurrentStateInPattern(patternsTree.getChild(currentPatternIndex));
}

void SlotMachineAudioProcessorEditor::refreshPatternTabs()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    if (patternsTree.getNumChildren() == 0)
    {
        auto pattern = processor.createDefaultPatternTree(defaultPatternNameForIndex(0));
        patternsTree.addChild(pattern, -1, nullptr);
        currentPatternIndex = 0;
        processor.setCurrentPatternIndex(currentPatternIndex);
    }

    juce::StringArray names;
    const int count = patternsTree.getNumChildren();
    names.ensureStorageAllocated(count);

    for (int i = 0; i < count; ++i)
    {
        auto child = patternsTree.getChild(i);
        juce::String name = child.getProperty(kPatternNameProperty).toString();
        if (name.isEmpty())
        {
            name = defaultPatternNameForIndex(i);
            child.setProperty(kPatternNameProperty, name, nullptr);
        }
        names.add(name);
    }

    if (names.isEmpty())
        names.add(defaultPatternNameForIndex(0));

    patternTabs.setTabs(names);
    patternTabs.setCurrentIndex(currentPatternIndex);
}

void SlotMachineAudioProcessorEditor::applyPatternTreeNow(const juce::ValueTree& pattern, bool allowTailRelease)
{
    juce::Array<int> failedSlots;
    processor.applyPatternTree(pattern, &failedSlots, allowTailRelease);

    refreshSlotFileLabels(failedSlots);
    showPatternWarning(failedSlots);
    repaint();
}

void SlotMachineAudioProcessorEditor::applyPattern(int index, bool updateTabs, bool saveExisting, bool deferIfRunning)
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    index = juce::jlimit(0, count - 1, index);

    if (saveExisting)
        saveCurrentPattern();

    auto pattern = patternsTree.getChild(index);
    if (!pattern.isValid())
        return;

    const bool isRunning = startToggle.getToggleState();
    const bool shouldDefer = deferIfRunning && isRunning;

    currentPatternIndex = index;
    processor.setCurrentPatternIndex(currentPatternIndex);

    if (updateTabs)
        patternTabs.setCurrentIndex(currentPatternIndex);

    if (shouldDefer)
    {
        pendingPatternTree = pattern;
        patternSwitchPending = true;
        return;
    }

    patternSwitchPending = false;
    pendingPatternTree = {};
    applyPatternTreeNow(pattern, isRunning);
}

void SlotMachineAudioProcessorEditor::showPatternWarning(const juce::Array<int>& failedSlots)
{
    if (failedSlots.isEmpty())
    {
        patternWarningLabel.setVisible(false);
        patternWarningCounter = 0;
        return;
    }

    juce::String text;
    if (failedSlots.size() == 1)
        text = "1 sample failed to load";
    else
        text = juce::String(failedSlots.size()) + " samples failed to load";

    juce::String patternName;
    if (patternsTree.isValid())
    {
        auto child = patternsTree.getChild(currentPatternIndex);
        patternName = child.getProperty(kPatternNameProperty).toString();
    }

    if (patternName.isNotEmpty())
        text = patternName + ": " + text;

    patternWarningLabel.setText(text, juce::dontSendNotification);
    patternWarningLabel.setVisible(true);
    patternWarningCounter = 300; // ~5 seconds at 60 Hz
}

void SlotMachineAudioProcessorEditor::refreshSlotFileLabels(const juce::Array<int>& failedSlots)
{
    auto getFilePropertyId = [](int slotIndex)
    {
        return juce::String("slot") + juce::String(slotIndex + 1) + "_File";
    };

    juce::ValueTree activePattern;
    if (patternsTree.isValid() && juce::isPositiveAndBelow(currentPatternIndex, patternsTree.getNumChildren()))
        activePattern = patternsTree.getChild(currentPatternIndex);

    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui)
            continue;

        const bool hasSample = processor.slotHasSample(i);
        juce::String path = processor.getSlotFilePath(i);

        if (path.isEmpty())
        {
            const auto propertyId = getFilePropertyId(i);

            if (activePattern.isValid())
                path = activePattern.getProperty(propertyId).toString();

            if (path.isEmpty())
            {
                const auto stateValue = apvts.state.getProperty(propertyId);
                if (!stateValue.isVoid())
                    path = stateValue.toString();
            }
        }

        juce::String label = "No file";

        if (path.isNotEmpty())
        {
            const bool failed = failedSlots.contains(i);
            juce::File f(path);
            const bool exists = f.existsAsFile();
            const juce::String fileName = f.getFileName().isNotEmpty() ? f.getFileName() : path;

            if (failed || !exists)
                label = fileName + " (missing)";
            else
                label = fileName;
        }

        ui->hasFile = hasSample;
        ui->fileLabel.setText(label, juce::dontSendNotification);
    }
}

void SlotMachineAudioProcessorEditor::handlePatternContextMenu(const juce::MouseEvent& e)
{
    if (fileDialogActive)
        return;

    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    juce::PopupMenu menu;
    const int patternCount = patternsTree.getNumChildren();

    menu.addItem(1, "New Pattern");
    menu.addItem(2, "Duplicate Pattern", patternCount > 0);
    menu.addItem(3, "Rename Pattern", patternCount > 0);
    menu.addItem(4, "Delete Pattern", patternCount > 1);
    menu.addSeparator();
    menu.addItem(5, "Import saved pattern", patternCount > 0);

    auto options = juce::PopupMenu::Options().withTargetComponent(&patternTabs);

    auto targetArea = patternTabs.getScreenBounds();
    targetArea.setX(e.getScreenX());
    targetArea.setWidth(1);

    options = options.withTargetScreenArea(targetArea);

    menu.showMenuAsync(options,
        [this](int result)
        {
            switch (result)
            {
            case 1: createNewPattern(); break;
            case 2: duplicateCurrentPattern(); break;
            case 3: renameCurrentPattern(); break;
            case 4: deleteCurrentPattern(); break;
            case 5: importPatternFromFile(); break;
            default: break;
            }
        });
}

void SlotMachineAudioProcessorEditor::reorderPatterns(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;

    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    if (!juce::isPositiveAndBelow(fromIndex, count) || !juce::isPositiveAndBelow(toIndex, count))
        return;

    saveCurrentPattern();

    auto child = patternsTree.getChild(fromIndex);
    if (!child.isValid())
        return;

    patternsTree.removeChild(fromIndex, nullptr);

    const int insertIndex = juce::jlimit(0, patternsTree.getNumChildren(), toIndex);
    if (insertIndex >= patternsTree.getNumChildren())
        patternsTree.addChild(child, -1, nullptr);
    else
        patternsTree.addChild(child, insertIndex, nullptr);

    const int newIndex = patternsTree.indexOf(child);

    if (currentPatternIndex == fromIndex)
        currentPatternIndex = newIndex;
    else if (fromIndex < toIndex && currentPatternIndex > fromIndex && currentPatternIndex <= toIndex)
        --currentPatternIndex;
    else if (fromIndex > toIndex && currentPatternIndex < fromIndex && currentPatternIndex >= toIndex)
        ++currentPatternIndex;

    currentPatternIndex = juce::jlimit(0, juce::jmax(0, patternsTree.getNumChildren() - 1), currentPatternIndex);

    processor.setCurrentPatternIndex(currentPatternIndex);
    refreshPatternTabs();
}

void SlotMachineAudioProcessorEditor::createNewPattern()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    saveCurrentPattern();

    const int newIndex = patternsTree.getNumChildren();
    auto pattern = processor.createDefaultPatternTree(defaultPatternNameForIndex(newIndex));
    patternsTree.addChild(pattern, -1, nullptr);

    currentPatternIndex = newIndex;
    processor.setCurrentPatternIndex(currentPatternIndex);
    refreshPatternTabs();
    applyPattern(currentPatternIndex, true, false);
}

void SlotMachineAudioProcessorEditor::duplicateCurrentPattern()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    saveCurrentPattern();

    const int newIndex = count;
    auto copy = patternsTree.getChild(currentPatternIndex).createCopy();
    copy.setProperty(kPatternNameProperty, defaultPatternNameForIndex(newIndex), nullptr);
    patternsTree.addChild(copy, -1, nullptr);

    currentPatternIndex = newIndex;
    processor.setCurrentPatternIndex(currentPatternIndex);
    refreshPatternTabs();
    patternTabs.setCurrentIndex(currentPatternIndex);
    juce::Array<int> none;
    refreshSlotFileLabels(none);
    showPatternWarning(none);
}

void SlotMachineAudioProcessorEditor::renameCurrentPattern()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 0)
        return;

    auto pattern = patternsTree.getChild(currentPatternIndex);
    juce::String currentName = pattern.getProperty(kPatternNameProperty).toString();
    if (currentName.isEmpty())
        currentName = defaultPatternNameForIndex(currentPatternIndex);

    auto component = std::make_unique<RenamePatternComponent>(currentName,
        [this, pattern, patternIndex = currentPatternIndex](bool accepted, juce::String newName) mutable
        {
            if (!accepted)
                return;

            newName = newName.trim();
            if (newName.isEmpty())
                newName = defaultPatternNameForIndex(patternIndex);

            pattern.setProperty(kPatternNameProperty, newName, nullptr);
            refreshPatternTabs();
            patternTabs.setCurrentIndex(patternIndex);
        });

    auto* componentPtr = component.get();
    componentPtr->setSize(260, 110);

    auto tabBounds = patternTabs.getTabBoundsInParent(currentPatternIndex);
    juce::Rectangle<int> anchorArea(0, 0, 1, 1);
    anchorArea.setCentre(tabBounds.getCentreX(), tabBounds.getBottom());

    auto& callout = juce::CallOutBox::launchAsynchronously(std::move(component),
        anchorArea, this);
    componentPtr->setCallOutBox(callout);
    componentPtr->focusEditor();
}

void SlotMachineAudioProcessorEditor::importPatternFromFile()
{
    if (fileDialogActive)
        return;

    auto chooser = std::make_shared<juce::FileChooser>("Import saved pattern", juce::File(), "*.xml");

    fileDialogActive = true;
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc) mutable
        {
            juce::ignoreUnused(chooser);
            fileDialogActive = false;

            auto file = fc.getResult();
            if (!file.existsAsFile())
                return;

            handlePatternImportFile(file);
        });
}

void SlotMachineAudioProcessorEditor::handlePatternImportFile(const juce::File& file)
{
    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
    if (xml == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
            "Import Pattern",
            "Unable to read the selected file.");
        return;
    }

    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
            "Import Pattern",
            "The selected file does not contain a valid pattern.");
        return;
    }

    static const juce::Identifier kPatternsNodeId("patterns");
    static const juce::Identifier kPatternNodeType("pattern");

    juce::Array<juce::ValueTree> importedPatterns;

    if (auto patternsNode = state.getChildWithName(kPatternsNodeId); patternsNode.isValid())
    {
        for (int i = 0; i < patternsNode.getNumChildren(); ++i)
        {
            auto child = patternsNode.getChild(i);
            if (child.hasType(kPatternNodeType))
                importedPatterns.add(child);
        }
    }
    else if (state.hasType(kPatternsNodeId))
    {
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            auto child = state.getChild(i);
            if (child.hasType(kPatternNodeType))
                importedPatterns.add(child);
        }
    }
    else if (state.hasType(kPatternNodeType))
    {
        importedPatterns.add(state);
    }

    if (importedPatterns.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
            "Import Pattern",
            "No saved patterns were found in the selected file.");
        return;
    }

    if (importedPatterns.size() == 1)
    {
        importPatternIntoCurrentTab(importedPatterns.getReference(0));
        return;
    }

    auto options = std::make_shared<std::vector<juce::ValueTree>>();
    options->reserve((size_t)importedPatterns.size());

    juce::PopupMenu selectionMenu;

    for (int i = 0; i < importedPatterns.size(); ++i)
    {
        auto pattern = importedPatterns.getReference(i);
        options->push_back(pattern);

        juce::String name = pattern.getProperty(kPatternNameProperty).toString();
        if (name.isEmpty())
            name = "Pattern " + juce::String(i + 1);

        selectionMenu.addItem(i + 1, name);
    }

    auto menuOptions = juce::PopupMenu::Options();

    auto tabBounds = patternTabs.getTabBoundsInParent(currentPatternIndex);
    auto screenArea = tabBounds;
    screenArea.setPosition(localPointToGlobal(tabBounds.getPosition()));

    if (screenArea.getWidth() > 0 && screenArea.getHeight() > 0)
        menuOptions = menuOptions.withTargetScreenArea(screenArea);
    else
        menuOptions = menuOptions.withTargetComponent(&patternTabs);

    selectionMenu.showMenuAsync(menuOptions,
        [this, options](int result)
        {
            if (result <= 0)
                return;

            const int index = result - 1;
            if (!juce::isPositiveAndBelow(index, (int)options->size()))
                return;

            importPatternIntoCurrentTab((*options)[(size_t)index]);
        });
}

void SlotMachineAudioProcessorEditor::importPatternIntoCurrentTab(const juce::ValueTree& patternTree)
{
    if (!patternTree.isValid())
        return;

    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    if (!patternsTree.isValid())
        return;

    if (patternsTree.getNumChildren() == 0)
        refreshPatternTabs();

    const int patternCount = patternsTree.getNumChildren();
    if (patternCount <= 0)
        return;

    currentPatternIndex = juce::jlimit(0, patternCount - 1, currentPatternIndex);

    saveCurrentPattern();

    auto currentPattern = patternsTree.getChild(currentPatternIndex);
    if (!currentPattern.isValid())
        return;

    juce::String currentName = currentPattern.getProperty(kPatternNameProperty).toString();
    if (currentName.isEmpty())
        currentName = defaultPatternNameForIndex(currentPatternIndex);

    auto importedCopy = patternTree.createCopy();
    importedCopy.setProperty(kPatternNameProperty, currentName, nullptr);

    patternsTree.removeChild(currentPatternIndex, nullptr);
    patternsTree.addChild(importedCopy, currentPatternIndex, nullptr);

    processor.setCurrentPatternIndex(currentPatternIndex);
    refreshPatternTabs();
    patternTabs.setCurrentIndex(currentPatternIndex);

    applyPatternTreeNow(importedCopy, startToggle.getToggleState());
    saveCurrentPattern();
}

void SlotMachineAudioProcessorEditor::clearExtraPatternsBeforeLoad()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    while (patternsTree.getNumChildren() > 1)
        patternsTree.removeChild(patternsTree.getNumChildren() - 1, nullptr);

    const int patternCount = patternsTree.getNumChildren();
    if (patternCount > 0)
    {
        currentPatternIndex = juce::jlimit(0, patternCount - 1, currentPatternIndex);
        processor.setCurrentPatternIndex(currentPatternIndex);
    }
    else
    {
        currentPatternIndex = 0;
        processor.setCurrentPatternIndex(currentPatternIndex);
    }

    refreshPatternTabs();
    patternTabs.setCurrentIndex(currentPatternIndex);
    juce::Array<int> none;
    refreshSlotFileLabels(none);
    showPatternWarning(none);
}

void SlotMachineAudioProcessorEditor::resetPatternsToSingleDefault()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    if (!patternsTree.isValid())
        return;

    while (patternsTree.getNumChildren() > 1)
        patternsTree.removeChild(patternsTree.getNumChildren() - 1, nullptr);

    if (patternsTree.getNumChildren() == 0)
    {
        auto pattern = processor.createDefaultPatternTree("A");
        patternsTree.addChild(pattern, -1, nullptr);
    }

    if (auto pattern = patternsTree.getChild(0); pattern.isValid())
        pattern.setProperty(kPatternNameProperty, "A", nullptr);

    currentPatternIndex = 0;
    processor.setCurrentPatternIndex(currentPatternIndex);

    refreshPatternTabs();
    patternTabs.setCurrentIndex(currentPatternIndex);
}

void SlotMachineAudioProcessorEditor::buttonClicked(juce::Button* b)
{
    if (b == &btnStart)
    {
        const bool nextState = !startToggle.getToggleState();
        setMasterRun(nextState);
        return;
    }

    // >>> Load sequence: Stop -> Load (initialization happens after confirming the preset)
    if (b == &btnLoad)
    {
        setMasterRun(false);
        doLoadPreset();
        return;
    }

    if (b == &btnSave) { doSavePreset();       return; }
    if (b == &btnResetLoop) { resetLoopTransport(); return; }
    if (b == &btnInitialize)
    {
        auto safeThis = juce::Component::SafePointer<SlotMachineAudioProcessorEditor>(this);

        confirmWarningWithContinue(this,
            "Initialize",
            "Initializing will clear all slots for the selected Tab. Would you like to Continue?",
            [safeThis]()
            {
                if (auto* editor = safeThis.getComponent())
                    editor->doResetAll();
            });

        return;
    }
    if (b == &btnReset)
    {
        auto safeThis = juce::Component::SafePointer<SlotMachineAudioProcessorEditor>(this);

        confirmWarningWithContinue(this,
            "Reset UI",
            "Resetting UI will delete all but the main Tab, and clear all slots. Would you like to Continue?",
            [safeThis]()
            {
                if (auto* editor = safeThis.getComponent())
                {
                    editor->resetPatternsToSingleDefault();
                    editor->doResetAll();
                }
            });

        return;
    }
    if (b == &btnOptions) { showOptionsDialog();  return; }

    if (b == &btnVisualizer)
    {
        setShowVisualizerParam(true);
        if (!vizWindow)
        {
            openVisualizerWindow();
            lastShowVisualizer = true;
        }
        return;
    }

    if (b == &btnUserManual)
    {
        openUserManual();
        return;
    }

    if (b == &btnAbout)
    {
        if (auto* existing = aboutDialog.getComponent())
        {
            if (auto* peer = existing->getPeer())
                peer->toFront(true);
            else
                existing->grabKeyboardFocus();
            return;
        }

        auto aboutContent = std::make_unique<AboutComponent>();
        aboutContent->setSize(420, 460);

        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "About Slot Machine";
        options.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
        options.content.setOwned(aboutContent.release());
        options.componentToCentreAround = this;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;

        if (auto* window = options.launchAsync())
        {
            aboutDialog = window;
            window->centreAroundComponent(this, window->getWidth(), window->getHeight());
        }

        return;
    }

    // ===== Export Audio (user-selected cycles) =====
    if (b == &btnExportAudio)
    {
        promptForExportCycles("Export Audio", 1,
            [this](int cycles)
            {
                beginAudioExportWithCycles(cycles);
            });
        return;
    }

    // ===== Export MIDI (user-selected cycles) =====
    if (b == &btnExportMidi)
    {
        promptForExportCycles("Export MIDI", 1,
            [this](int cycles)
            {
                beginMidiExportWithCycles(cycles);
            });
        return;
    }

    // Per-slot file load / clear / solo
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui) continue;

        // CLEAR sample
        if (b == &ui->clearBtn)
        {
            processor.clearSlot(i, startToggle.getToggleState()); // remove sample + path in processor
            ui->hasFile = false;                              // reflect in UI
            ui->fileLabel.setText("No file", juce::dontSendNotification);
            ui->glow = 0.0f;                                  // optional: calm the halo
            ui->phase = 0.0f;                                 // optional: reset bar
            ui->lastHitCounter = 0;                           // optional: reset hit pulse
            repaint();
            return;
        }

        // existing: LOAD sample
        if (b == &ui->fileBtn)
        {
            // ... your existing FileChooser code remains unchanged ...
        }

        // existing: mutually exclusive Solo
        if (b == &ui->soloBtn)
        {
            // ... your existing solo exclusivity code ...
        }
    }


    // Slot events (file load + solo exclusivity)
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui) continue;

        // Per-slot file load
        if (b == &ui->fileBtn)
        {
            auto chooser = std::make_shared<juce::FileChooser>(
                "Select audio file", juce::File(), "*.wav;*.aiff;*.aif;*.flac");

            fileDialogActive = true;
            chooser->launchAsync(juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
                [this, i, chooser](const juce::FileChooser& fc) mutable
                {
                    juce::ignoreUnused(chooser);
                    fileDialogActive = false;
                    handleSlotFileSelection(i, fc.getResult());
                });
            return;
        }

        // Mute/Solo interactions
        if (b == &ui->muteBtn)
        {
            const bool nowOn = ui->muteBtn.getToggleState();
            if (nowOn)
            {
                if (auto* soloParam = dynamic_cast<juce::AudioParameterBool*>(
                        apvts.getParameter("slot" + juce::String(i + 1) + "_Solo")))
                {
                    soloParam->beginChangeGesture();
                    *soloParam = false;
                    soloParam->endChangeGesture();
                }
            }
            return;
        }

        if (b == &ui->soloBtn)
        {
            const bool nowOn = ui->soloBtn.getToggleState();
            if (nowOn)
            {
                if (auto* muteParam = dynamic_cast<juce::AudioParameterBool*>(
                        apvts.getParameter("slot" + juce::String(i + 1) + "_Mute")))
                {
                    muteParam->beginChangeGesture();
                    *muteParam = false;
                    muteParam->endChangeGesture();
                }

                for (int j = 0; j < kNumSlots; ++j)
                {
                    if (j == i) continue;
                    if (auto* soloParam = dynamic_cast<juce::AudioParameterBool*>(
                            apvts.getParameter("slot" + juce::String(j + 1) + "_Solo")))
                    {
                        soloParam->beginChangeGesture();
                        *soloParam = false;
                        soloParam->endChangeGesture();
                    }
                }
            }
            return;
        }
    }
}

void SlotMachineAudioProcessorEditor::showOptionsDialog()
{
    auto content = std::make_unique<OptionsComponent>(apvts, [this](float newScale)
        {
            applySlotScale(newScale);
        });
    content->setSize(640, 668);

    juce::DialogWindow::LaunchOptions opt;
    opt.dialogTitle = "Options";
    opt.content.setOwned(content.release());
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.componentToCentreAround = this;
    opt.resizable = true;
    opt.dialogBackgroundColour = juce::Colours::black;

    if (auto* dlg = opt.launchAsync())
        dlg->setResizeLimits(480, 668, 2000, 1368);
}

void SlotMachineAudioProcessorEditor::promptForExportCycles(const juce::String& dialogTitle,
    int defaultCycles,
    std::function<void(int)> onConfirm)
{
    if (auto* existing = exportCyclesPromptWindow.getComponent())
    {
        if (auto* peer = existing->getPeer())
            peer->toFront(true);
        else
            existing->grabKeyboardFocus();
        return;
    }

    auto editorSafe = juce::Component::SafePointer<SlotMachineAudioProcessorEditor>(this);
    auto dialogContent = std::make_unique<ExportCyclesDialog>(
        defaultCycles,
        [editorSafe, handler = std::move(onConfirm)](int cycles) mutable
        {
            if (editorSafe != nullptr)
            {
                editorSafe->exportCyclesPromptWindow = nullptr;

                if (handler)
                    handler(cycles);
            }
        },
        [editorSafe]()
        {
            if (editorSafe != nullptr)
                editorSafe->exportCyclesPromptWindow = nullptr;
        });

    dialogContent->setSize(360, 180);

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = dialogTitle;
    options.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.content.setOwned(dialogContent.release());
    options.componentToCentreAround = this;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    if (auto* window = options.launchAsync())
    {
        exportCyclesPromptWindow = window;
        window->centreAroundComponent(this, window->getWidth(), window->getHeight());
    }
}

void SlotMachineAudioProcessorEditor::beginAudioExportWithCycles(int cyclesRequested)
{
    if (cyclesRequested <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Export Audio",
            "Please enter a positive whole number of cycles.");
        return;
    }

    auto chooser = std::make_shared<juce::FileChooser>(
        "Export " + juce::String(cyclesRequested) + "-cycle audio file",
        juce::File(),
        "*.wav");

    fileDialogActive = true;
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser, cyclesRequested](const juce::FileChooser& fc) mutable
        {
            juce::ignoreUnused(chooser);
            fileDialogActive = false;

            auto file = fc.getResult();
            if (file.getFullPathName().isEmpty())
                return;

            if (!file.hasFileExtension(".wav"))
                file = file.withFileExtension(".wav");

            juce::String error;
            if (processor.exportAudioCycles(file, cyclesRequested, error))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Export Audio",
                    "Saved: " + file.getFullPathName());
            }
            else
            {
                if (error.isEmpty())
                    error = "Unable to export audio.";

                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Export Audio",
                    error);
            }
        });
}

void SlotMachineAudioProcessorEditor::beginMidiExportWithCycles(int cyclesRequested)
{
    if (cyclesRequested <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Export MIDI",
            "Please enter a positive whole number of cycles.");
        return;
    }

    using namespace MidiExport;

    std::vector<SlotDef> active;
    active.reserve(kNumSlots);

    const int timingMode = Opt::getInt(apvts, "optTimingMode", 0);

    bool anySolo = false;
    std::vector<bool> soloMask(kNumSlots, false);
    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool solo = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Solo")->load();
        soloMask[(size_t)i] = solo;
        anySolo = anySolo || solo;
    }

    for (int i = 0; i < kNumSlots; ++i)
    {
        const bool mute = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Mute")->load();
        const float rate = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Rate");
        int count = 4;
        if (auto* countParam = apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Count"))
            count = juce::jlimit(1, kMaxBeatsPerSlot, (int)std::round(countParam->load()));
        const float gainPercent = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_Gain");
        const float midiChoice = *apvts.getRawParameterValue("slot" + juce::String(i + 1) + "_MidiChannel");

        if (mute)
            continue;
        if (anySolo && !soloMask[(size_t)i])
            continue;
        if (!processor.slotHasSample(i))
            continue;

        SlotDef s;
        s.index = i;
        s.note = 60;
        s.channel = juce::jlimit(1, 16, 1 + (int)std::round(midiChoice));
        s.rate = juce::jmax(0.0001f, rate);
        s.count = juce::jmax(1, count);
        s.gain = gainPercent * 0.01f;
        active.push_back(s);
    }

    if (active.empty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Export MIDI",
            "No active slots to export (check Mute/Solo & samples).");
        return;
    }

    const double bpm = (double)*apvts.getRawParameterValue("masterBPM");
    const int ppq = 9600;

    const int maxDen = 32;
    int cycleBeats = 1;
    if (timingMode == 0)
    {
        for (const auto& sdef : active)
        {
            int num = 0;
            int den = 1;
            MidiExport::approximateRational(sdef.rate, maxDen, num, den);
            if (num <= 0)
                continue;

            int g = MidiExport::igcd(num, den);
            num /= g;
            den /= g;
            cycleBeats = MidiExport::ilcm(cycleBeats, den);
        }
    }
    else
    {
        for (const auto& sdef : active)
            cycleBeats = MidiExport::ilcm(cycleBeats, juce::jmax(1, sdef.count));
    }

    if (cycleBeats <= 0 || cycleBeats > 512)
        cycleBeats = juce::jlimit(1, 512, cycleBeats);

    const int cycleTicks = cycleBeats * ppq;
    const int maxCycles = juce::jmax(1, std::numeric_limits<int>::max() / juce::jmax(1, cycleTicks));
    const int cyclesToExport = juce::jlimit(1, maxCycles, cyclesRequested);

    if (cyclesToExport != cyclesRequested)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Export MIDI",
            "The requested number of cycles is too large for a MIDI file. Exporting "
                + juce::String(cyclesToExport)
                + " cycles instead.");
    }

    const int totalTicks = cycleTicks * cyclesToExport;

    juce::MidiMessageSequence seq;

    if (bpm > 0.0)
    {
        const double usPerQuarter = 60000000.0 / bpm;
        seq.addEvent(juce::MidiMessage::tempoMetaEvent((int)std::round(usPerQuarter)));
    }

    seq.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 2));

    for (const auto& sdef : active)
    {
        const int vel = juce::jlimit(1, 127, (int)std::round(sdef.gain * 127.0f));
        const int noteLength = juce::jmax(1, ppq / 64);

        if (timingMode == 0)
        {
            int num = 0;
            int den = 1;
            MidiExport::approximateRational(sdef.rate, maxDen, num, den);
            int g = MidiExport::igcd(num, den);
            num /= g;
            den /= g;

            if (num <= 0)
                continue;

            const int hits = (int)((num * cycleBeats) / den);
            const double invRate = 1.0 / (double)sdef.rate;

            for (int hit = 0; hit < hits; ++hit)
            {
                const double beat = (double)hit * invRate;
                const double tick = beat * (double)ppq;
                const int baseTick = juce::jlimit(0, cycleTicks - 1, (int)std::llround(tick));

                for (int cycle = 0; cycle < cyclesToExport; ++cycle)
                {
                    const int cycleOffset = cycle * cycleTicks;
                    const int startTick = juce::jlimit(0, totalTicks - 1, cycleOffset + baseTick);
                    const int offTick = juce::jmin(totalTicks, startTick + noteLength);

                    seq.addEvent(juce::MidiMessage::noteOn(sdef.channel, sdef.note, (juce::uint8)vel), startTick);
                    seq.addEvent(juce::MidiMessage::noteOff(sdef.channel, sdef.note), offTick);
                }
            }
        }
        else
        {
            const int count = juce::jmax(1, sdef.count);
            const double stepBeats = (double)cycleBeats / (double)count;

            for (int n = 0; n < count; ++n)
            {
                const double beat = (double)n * stepBeats;
                const int baseTick = juce::jlimit(0, cycleTicks - 1, (int)std::llround(beat * (double)ppq));

                for (int cycle = 0; cycle < cyclesToExport; ++cycle)
                {
                    const int cycleOffset = cycle * cycleTicks;
                    const int startTick = juce::jlimit(0, totalTicks - 1, cycleOffset + baseTick);
                    const int offTick = juce::jmin(totalTicks, startTick + noteLength);

                    seq.addEvent(juce::MidiMessage::noteOn(sdef.channel, sdef.note, (juce::uint8)vel), startTick);
                    seq.addEvent(juce::MidiMessage::noteOff(sdef.channel, sdef.note), offTick);
                }
            }
        }
    }

    seq.addEvent(juce::MidiMessage::endOfTrack(), totalTicks);

    const juce::String cycleLabel = cyclesToExport == 1 ? "1-cycle" : juce::String(cyclesToExport) + "-cycle";
    auto chooser = std::make_shared<juce::FileChooser>(
        "Export " + cycleLabel + " MIDI file",
        juce::File(),
        "*.mid");

    fileDialogActive = true;
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this, seq, ppq, chooser](const juce::FileChooser& fc) mutable
        {
            fileDialogActive = false;

            auto f = fc.getResult();
            if (f.getFullPathName().isEmpty())
                return;

            if (!f.hasFileExtension(".mid"))
                f = f.withFileExtension(".mid");

            juce::MidiFile mf;
            mf.setTicksPerQuarterNote(ppq);
            mf.addTrack(seq);

            juce::FileOutputStream os(f);
            if (os.openedOk())
            {
                mf.writeTo(os);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Export MIDI",
                    "Saved: " + f.getFullPathName());
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Export MIDI",
                    "Couldn't write file:\n" + f.getFullPathName());
            }
        });
}

void SlotMachineAudioProcessorEditor::setShowVisualizerParam(bool shouldShow)
{
    if (auto* param = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("optShowVisualizer")))
    {
        if (param->get() == shouldShow)
            return;

        param->beginChangeGesture();
        *param = shouldShow;
        param->endChangeGesture();
        saveOptionsToDisk(apvts);
    }
}

void SlotMachineAudioProcessorEditor::openVisualizerWindow()
{
    if (vizWindow)
    {
        if (auto* peer = vizWindow->getPeer())
            peer->toFront(true);
        else
            vizWindow->toFront(true);
        return;
    }

    vizComponent = std::make_unique<PolyrhythmVizComponent>(processor, apvts);
    auto* componentPtr = vizComponent.release();

    auto window = std::make_unique<VisualizerWindow>(*this);
    window->setContentOwned(componentPtr, true);
    window->centreWithSize(640, 640);
    window->setVisible(true);
    window->toFront(true);

    vizWindow = std::move(window);
}

void SlotMachineAudioProcessorEditor::closeVisualizerWindow()
{
    if (vizWindow)
    {
        vizWindow->setVisible(false);
        vizWindow.reset();
    }

    vizComponent.reset();
}

void SlotMachineAudioProcessorEditor::handleVisualizerWindowCloseRequest()
{
    closeVisualizerWindow();
    lastShowVisualizer = false;
    setShowVisualizerParam(false);
}

void SlotMachineAudioProcessorEditor::openUserManual()
{
    auto manualFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("SlotMachine-UserManual.html");

    manualFile.getParentDirectory().createDirectory();

    if (!manualFile.replaceWithData(BinaryData::SlotMachineUserManual_html,
                                    static_cast<size_t>(BinaryData::SlotMachineUserManual_htmlSize)))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "User Manual",
            "Unable to access the embedded User Manual.");
        return;
    }

#if JUCE_WEB_BROWSER
    if (juce::URL(manualFile).launchInDefaultBrowser())
        return;
#endif

    if (!manualFile.startAsProcess())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "User Manual",
            "Unable to open the embedded User Manual in a browser.");
    }
}

void SlotMachineAudioProcessorEditor::setMasterRun(bool shouldRun)
{
    if (auto* runParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("masterRun")))
    {
        if (runParam->get() != shouldRun)
        {
            runParam->beginChangeGesture();
            *runParam = shouldRun;
            runParam->endChangeGesture();
        }
    }

    startToggle.setToggleState(shouldRun, juce::dontSendNotification);

    const auto glowColour = Opt::rgbParam(apvts, "optGlowColor", 0x6994FC, 1.0f);
    const auto pulseColour = Opt::rgbParam(apvts, "optPulseColor", 0xD3CFE4, 1.0f);
    const float glowAlpha = Opt::getFloat(apvts, "optGlowAlpha", 0.431f);
    const float glowWidth = Opt::getFloat(apvts, "optGlowWidth", 1.34f);

    updateStartButtonVisuals(shouldRun, glowColour, pulseColour, glowAlpha, glowWidth);
    cachedStartGlowColour = glowColour;
    cachedStartPulseColour = pulseColour;
    cachedStartGlowAlpha = glowAlpha;
    cachedStartGlowWidth = glowWidth;
    lastStartToggleState = shouldRun;

    if (shouldRun)
    {
        animateStartButton(glowColour, pulseColour);
    }
    else
    {
        startButtonAnimPhase = 0.0f;
    }
}

void SlotMachineAudioProcessorEditor::updateStartButtonVisuals(bool shouldRun,
    juce::Colour glowColour,
    juce::Colour pulseColour,
    float glowAlpha,
    float glowWidth)
{
    juce::ignoreUnused(pulseColour);

    if (shouldRun)
    {
        if (btnStart.getButtonText() != "Stop")
            btnStart.setButtonText("Stop");

        if (startButtonGlowEnabled)
        {
            btnStart.setComponentEffect(nullptr);
            startButtonGlowEnabled = false;
        }

        auto baseColour = glowColour.withAlpha(juce::jlimit(0.4f, 1.0f, glowAlpha + 0.45f));
        btnStart.setColour(juce::TextButton::textColourOffId, baseColour);
        btnStart.setColour(juce::TextButton::textColourOnId, baseColour);
    }
    else
    {
        if (btnStart.getButtonText() != "Start")
            btnStart.setButtonText("Start");

        const float glowRadius = juce::jlimit(6.0f, 42.0f, glowWidth * 3.0f);
        const float glowIntensity = juce::jlimit(0.2f, 0.95f, glowAlpha + 0.35f);
        startButtonGlow.setGlowProperties(glowRadius, glowColour.withAlpha(glowIntensity));

        if (!startButtonGlowEnabled)
        {
            btnStart.setComponentEffect(&startButtonGlow);
            startButtonGlowEnabled = true;
        }

        auto textColour = glowColour.withAlpha(juce::jlimit(0.6f, 1.0f, glowAlpha + 0.55f));
        btnStart.setColour(juce::TextButton::textColourOffId, textColour);
        btnStart.setColour(juce::TextButton::textColourOnId, textColour);
    }

    btnStart.repaint();
}

void SlotMachineAudioProcessorEditor::animateStartButton(juce::Colour glowColour, juce::Colour pulseColour)
{
    startButtonAnimPhase += 0.04f;
    if (startButtonAnimPhase > juce::MathConstants<float>::twoPi)
        startButtonAnimPhase -= juce::MathConstants<float>::twoPi;

    const float mix = 0.5f * (1.0f + std::sin(startButtonAnimPhase));
    auto blended = glowColour.interpolatedWith(pulseColour, mix);

    const float brightness = 0.55f + 0.45f * (0.5f * (1.0f + std::sin(startButtonAnimPhase * 0.75f + juce::MathConstants<float>::halfPi)));
    blended = blended.withAlpha(juce::jlimit(0.35f, 1.0f, brightness));

    btnStart.setColour(juce::TextButton::textColourOffId, blended);
    btnStart.setColour(juce::TextButton::textColourOnId, blended);
    btnStart.repaint();
}

void SlotMachineAudioProcessorEditor::updateSliderKnobColours(juce::Colour pulseColour)
{
    if (pulseColour == cachedKnobPulseColour)
        return;

    masterBPM.setColour(juce::Slider::thumbColourId, pulseColour);

    for (auto& slot : slots)
    {
        if (!slot)
            continue;

        slot->rate.setColour(juce::Slider::thumbColourId, pulseColour);
        slot->gain.setColour(juce::Slider::thumbColourId, pulseColour);
        slot->decay.setColour(juce::Slider::thumbColourId, pulseColour);
    }

    cachedKnobPulseColour = pulseColour;
}

// ===== Preset Save / Load / Initialize =====
void SlotMachineAudioProcessorEditor::doSavePreset()
{
    saveCurrentPattern();
    auto chooser = std::make_shared<juce::FileChooser>("Save preset", juce::File(), "*.xml");
    fileDialogActive = true;
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc) mutable
        {
            juce::ignoreUnused(chooser);
            fileDialogActive = false;
            auto f = fc.getResult();
            if (f.getFullPathName().isNotEmpty())
            {
                if (!f.hasFileExtension(".xml"))
                    f = f.withFileExtension(".xml");
                auto state = processor.copyStateWithVersion();
                if (auto xml = state.createXml())
                    xml->writeTo(f);
            }
        });
}

void SlotMachineAudioProcessorEditor::doLoadPreset()
{
    saveCurrentPattern();
    auto chooser = std::make_shared<juce::FileChooser>("Load preset", juce::File(), "*.xml");
    fileDialogActive = true;
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc) mutable
        {
            juce::ignoreUnused(chooser);
            fileDialogActive = false;
            auto f = fc.getResult();
            if (!f.existsAsFile()) return;

            std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(f));
            if (xml == nullptr) return;

            auto newState = juce::ValueTree::fromXml(*xml);
            if (!newState.isValid())
                return;

            doResetAll();

            clearExtraPatternsBeforeLoad();

            const float previousSlotScaleParam = Opt::getFloat(apvts, "optSlotScale", slotScale);
            const bool presetHasSlotScale = newState.hasProperty("optSlotScale");

            // Load all parameters + properties into the APVTS
            apvts.replaceState(newState);
            processor.upgradeLegacySlotParameters();

            if (!presetHasSlotScale)
            {
                if (auto* slotScaleParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("optSlotScale")))
                {
                    slotScaleParam->beginChangeGesture();
                    slotScaleParam->setValueNotifyingHost(slotScaleParam->range.convertTo0to1(previousSlotScaleParam));
                    slotScaleParam->endChangeGesture();
                    applySlotScale(previousSlotScaleParam);
                }
            }
            else
            {
                applySlotScale(Opt::getFloat(apvts, "optSlotScale", slotScale));
            }

            patternsTree = processor.getPatternsTree();
            const int patternCount = patternsTree.getNumChildren();
            if (patternCount > 0)
            {
                currentPatternIndex = juce::jlimit(0, patternCount - 1, processor.getCurrentPatternIndex());
                processor.setCurrentPatternIndex(currentPatternIndex);
                refreshPatternTabs();
                applyPattern(currentPatternIndex, true, false);
            }
            else
            {
                currentPatternIndex = 0;
                refreshPatternTabs();
                juce::Array<int> none;
                refreshSlotFileLabels(none);
                showPatternWarning(none);
            }

            // Persist options immediately too (Standalone)
            saveOptionsToDisk(apvts);
        });
}

void SlotMachineAudioProcessorEditor::deleteCurrentPattern()
{
    if (!patternsTree.isValid())
        patternsTree = processor.getPatternsTree();

    const int count = patternsTree.getNumChildren();
    if (count <= 1)
        return;

    saveCurrentPattern();

    const int indexToRemove = juce::jlimit(0, count - 1, currentPatternIndex);
    patternsTree.removeChild(indexToRemove, nullptr);

    const int remaining = patternsTree.getNumChildren();
    currentPatternIndex = juce::jlimit(0, remaining - 1, indexToRemove);

    refreshPatternTabs();
    applyPattern(currentPatternIndex, true, false);
}

void SlotMachineAudioProcessorEditor::doResetAll(bool persistOptions)
{
    // Reset every parameter to its default via the processor’s parameter list
    const auto& params = processor.getParameters();
    for (auto* p : params)
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            if (isOptionParameter(rp->getParameterID()))
                continue;

            rp->beginChangeGesture();
            rp->setValueNotifyingHost(rp->getDefaultValue()); // normalized 0..1 default
            rp->endChangeGesture();
        }
    }

    // Clear all samples & file path properties
    processor.clearAllSlots();

    for (int i = 0; i < kNumSlots; ++i)
    {
        if (slots[(size_t)i])
        {
            slots[(size_t)i]->hasFile = false;
            slots[(size_t)i]->fileLabel.setText("No file", juce::dontSendNotification);
            slots[(size_t)i]->glow = 0.0f;
            slots[(size_t)i]->phase = 0.0f;
            slots[(size_t)i]->lastHitCounter = 0;
        }
    }

    // Reset phases (soft)
    processor.resetAllPhases(false);

    resetProgressVisuals();

    // Persist options (Standalone fallback)
    if (persistOptions)
        saveOptionsToDisk(apvts);

    saveCurrentPattern();
}

void SlotMachineAudioProcessorEditor::resetLoopTransport()
{
    processor.resetAllPhases(true);

    for (int i = 0; i < kNumSlots; ++i)
    {
        if (slots[(size_t)i])
            slots[(size_t)i]->lastHitCounter = processor.getSlotHitCounter(i);
    }

    resetProgressVisuals();
}

void SlotMachineAudioProcessorEditor::resetProgressVisuals()
{
    masterPhase = 0.0f;
    lastPhase = 0.0f;
    cycleFlash = 0.0f;
    startButtonAnimPhase = 0.0f;

    for (auto& slot : slots)
    {
        if (!slot)
            continue;

        slot->phase = 0.0f;
        slot->glow = 0.0f;
    }

    if (patternWarningCounter > 0)
    {
        --patternWarningCounter;
        if (patternWarningCounter == 0)
            patternWarningLabel.setVisible(false);
    }

    repaint();
}


void SlotMachineAudioProcessorEditor::timerCallback()
{
    const float currentScaleParam = Opt::getFloat(apvts, "optSlotScale", slotScale);
    if (std::abs(currentScaleParam - slotScale) > 0.0001f)
        applySlotScale(currentScaleParam);

    bool showVisualizer = false;
    if (auto* showParam = apvts.getRawParameterValue("optShowVisualizer"))
        showVisualizer = showParam->load() >= 0.5f;

    if (showVisualizer != lastShowVisualizer)
    {
        lastShowVisualizer = showVisualizer;
        if (showVisualizer)
            openVisualizerWindow();
        else
            closeVisualizerWindow();
    }

    const bool isRunning = startToggle.getToggleState();
    const auto glowColour = Opt::rgbParam(apvts, "optGlowColor", 0x6994FC, 1.0f);
    const auto pulseColour = Opt::rgbParam(apvts, "optPulseColor", 0xD3CFE4, 1.0f);
    const float glowAlpha = Opt::getFloat(apvts, "optGlowAlpha", 0.431f);
    const float glowWidth = Opt::getFloat(apvts, "optGlowWidth", 1.34f);

    updateSliderKnobColours(pulseColour);

    if (isRunning != lastStartToggleState
        || glowColour != cachedStartGlowColour
        || pulseColour != cachedStartPulseColour
        || std::abs(glowAlpha - cachedStartGlowAlpha) > 0.0001f
        || std::abs(glowWidth - cachedStartGlowWidth) > 0.0001f)
    {
        updateStartButtonVisuals(isRunning, glowColour, pulseColour, glowAlpha, glowWidth);
        lastStartToggleState = isRunning;
        cachedStartGlowColour = glowColour;
        cachedStartPulseColour = pulseColour;
        cachedStartGlowAlpha = glowAlpha;
        cachedStartGlowWidth = glowWidth;

        if (!isRunning)
            startButtonAnimPhase = 0.0f;
    }

    if (isRunning)
        animateStartButton(glowColour, pulseColour);

    // 0..1 over full polyrhythmic cycle
    const float p = juce::jlimit(0.0f, 1.0f, (float)processor.getMasterPhase());

    // Detect wrap (phase jumped backwards a bit)
    const bool wrapped = (p + 0.02f) < lastPhase; // small hysteresis
    if (wrapped)
        cycleFlash = 1.0f;                        // start flash

    if (patternSwitchPending && (!isRunning || wrapped))
    {
        if (pendingPatternTree.isValid())
            applyPatternTreeNow(pendingPatternTree, isRunning);

        patternSwitchPending = false;
        pendingPatternTree = {};
    }

    // Decay flash envelope @ ~60 Hz
    cycleFlash = juce::jmax(0.0f, cycleFlash * 0.88f - 0.01f);

    lastPhase = p;
    masterPhase = p; // used by paint() for the master bar

    // ---- per-slot UI polling ----
    const int timingMode = Opt::getInt(apvts, "optTimingMode", 0);

    if (timingMode != lastTimingMode)
    {
        for (int i = 0; i < kNumSlots; ++i)
            if (auto* slot = slots[(size_t)i].get())
                initialiseSlotTimingPair(i, *slot);

        lastTimingMode = timingMode;
    }

    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui) continue;

        ui->phase = (float)processor.getSlotPhase(i);

        const uint32_t hits = processor.getSlotHitCounter(i);
        if (hits != ui->lastHitCounter)
        {
            ui->lastHitCounter = hits;
            ui->glow = 1.0f; // pulse on hit
        }

        // simple glow decay
        ui->glow = juce::jmax(0.0f, ui->glow - 0.06f);
        ui->hasFile = processor.slotHasSample(i);

        const bool beatsPerCycleMode = (timingMode == 1);
        const bool countEnabled = beatsPerCycleMode;
        const bool rateEnabled = !beatsPerCycleMode;

        if (ui->count.isEnabled() != countEnabled)
            ui->count.setEnabled(countEnabled);
        if (ui->rate.isEnabled() != rateEnabled)
            ui->rate.setEnabled(rateEnabled);

        ui->count.setAlpha(countEnabled ? 1.0f : 0.35f);
        ui->rate.setAlpha(rateEnabled ? 1.0f : 0.35f);
    }

    repaint();
}

void SlotMachineAudioProcessorEditor::handleSlotRateChanged(int slotIndex, SlotUI& ui)
{
    if (ui.syncingFromCount) return;

    const float rateValue   = (float) ui.rate.getValue();
    const int   desiredCount = convertRateToCount(rateValue);
    const juce::String paramId = "slot" + juce::String(slotIndex + 1) + "_Count";

    if (auto* p = apvts.getParameter(paramId)) // juce::RangedAudioParameter*
    {
        // Prevent feedback loop into the other handler
        juce::ScopedValueSetter<bool> guard(ui.syncingFromRate, true);

        // Normalise real value -> 0..1 using the parameter's own mapping
        const float normalised = p->convertTo0to1((float) desiredCount);

        p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        p->endChangeGesture();
    }
}

void SlotMachineAudioProcessorEditor::handleSlotCountChanged(int slotIndex, SlotUI& ui)
{
    if (ui.syncingFromRate) return;

    const int   countValue   = (int) std::round(ui.count.getValue());
    const float desiredRate  = convertCountToRate(countValue);
    const juce::String paramId = "slot" + juce::String(slotIndex + 1) + "_Rate";

    if (auto* p = apvts.getParameter(paramId)) // juce::RangedAudioParameter*
    {
        juce::ScopedValueSetter<bool> guard(ui.syncingFromCount, true);

        const float normalised = p->convertTo0to1(desiredRate);

        p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        p->endChangeGesture();
    }
}

void SlotMachineAudioProcessorEditor::initialiseSlotTimingPair(int slotIndex, SlotUI& ui)
{
    const int timingMode = Opt::getInt(apvts, "optTimingMode", 0);

    if (timingMode == 1)
    {
        handleSlotCountChanged(slotIndex, ui);
    }
    else
    {
        handleSlotRateChanged(slotIndex, ui);
    }
}

void SlotMachineAudioProcessorEditor::handleMasterTap()
{
    constexpr double tapWindowSeconds = 6.0;
    constexpr double minimumSpanSeconds = 3.0;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    masterTapTimes.push_back(now);

    while (!masterTapTimes.empty() && (now - masterTapTimes.front()) > tapWindowSeconds)
        masterTapTimes.pop_front();

    if (masterTapTimes.size() < 3)
        return;

    const double span = masterTapTimes.back() - masterTapTimes.front();
    if (span < minimumSpanSeconds)
        return;

    const double minInterval = 60.0 / (double)masterBPM.getMaximum();
    const double maxInterval = 60.0 / (double)masterBPM.getMinimum();

    double intervalSum = 0.0;
    int validIntervals = 0;

    for (size_t i = 1; i < masterTapTimes.size(); ++i)
    {
        const double diff = masterTapTimes[i] - masterTapTimes[i - 1];
        if (diff < minInterval || diff > maxInterval)
            continue;

        intervalSum += diff;
        ++validIntervals;
    }

    if (validIntervals == 0)
        return;

    const double averageInterval = intervalSum / (double)validIntervals;
    double bpm = 60.0 / averageInterval;
    bpm = juce::jlimit(masterBPM.getMinimum(), masterBPM.getMaximum(), bpm);

    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("masterBPM")))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1((float)bpm));
        param->endChangeGesture();
    }
}

namespace
{
    bool componentContainsScreenPoint(const juce::Component& component, juce::Point<int> screenPoint)
    {
        if (!component.isShowing())
            return false;

        return component.getScreenBounds().contains(screenPoint);
    }
}

void SlotMachineAudioProcessorEditor::mouseUp(const juce::MouseEvent& e)
{
    const juce::Point<int> screenPos(e.getScreenX(), e.getScreenY());

    if (componentContainsScreenPoint(masterLabel, screenPos))
    {
        if (e.mouseWasClicked())
            handleMasterTap();
        return;
    }

    int clickedIndex = -1;
    for (int i = 0; i < kNumSlots; ++i)
    {
        auto* ui = slots[(size_t)i].get();
        if (!ui)
            continue;

        if (componentContainsScreenPoint(ui->group, screenPos))
        {
            clickedIndex = i;
            break;
        }
    }

    if (clickedIndex < 0)
        return;

    auto& U = *slots[(size_t)clickedIndex];

    auto isInteractiveHit = [screenPos](juce::Component& c)
    {
        return componentContainsScreenPoint(c, screenPos);
    };

    if (isInteractiveHit(U.fileBtn) || isInteractiveHit(U.clearBtn) || isInteractiveHit(U.fileLabel)
        || isInteractiveHit(U.muteBtn) || isInteractiveHit(U.soloBtn)
        || isInteractiveHit(U.muteLabel) || isInteractiveHit(U.soloLabel)
        || isInteractiveHit(U.rate) || isInteractiveHit(U.gain) || isInteractiveHit(U.decay))
        return;

    if (!processor.slotHasSample(clickedIndex))
        return;

    processor.requestManualTrigger(clickedIndex);
}
