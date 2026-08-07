/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

#include "SurgeSynthEditor.h"
#include "SurgeSynthProcessor.h"
#include "SurgeImageStore.h"
#include "SurgeImage.h"
#include "SurgeGUIEditor.h"
#include "SurgeGUIUtils.h"
#include "SurgeJUCELookAndFeel.h"
#include "RuntimeFont.h"
#include "AccessibleHelpers.h"
#include "PatchFileHeaderStructs.h"
#include "SurgeXTBinary.h"
#include "dsp/oscillators/ClassicOscillator.h"
#include "sst/basic-blocks/mechanics/endian-ops.h"
#include <version.h>
#include <cstring>
#include "gui/widgets/CurrentFxDisplay.h"
#include "gui/widgets/MainFrame.h"
#include "gui/overlays/Alert.h"
#include <chrono>
#include <cstdint>
#include <set>

struct AssistantPromptEditor : public juce::TextEditor
{
    using juce::TextEditor::TextEditor;

    std::function<void(bool)> onFocusChanged = [](bool) {};

    void focusGained(FocusChangeType cause) override
    {
        juce::TextEditor::focusGained(cause);
        onFocusChanged(true);
    }

    void focusLost(FocusChangeType cause) override
    {
        juce::TextEditor::focusLost(cause);
        onFocusChanged(false);
    }
};

struct AssistantGearButton : public juce::TextButton
{
    void paintButton(juce::Graphics &graphics, bool isMouseOverButton, bool isButtonDown) override
    {
        auto highlighted = isMouseOverButton || isButtonDown;
        graphics.setColour(findColour(highlighted ? buttonOnColourId : buttonColourId));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        graphics.setColour(findColour(highlighted ? textColourOnId : textColourOffId));
        graphics.setFont(juce::Font(35.0f));
        graphics.drawText(juce::String::charToString(0x2699), getLocalBounds().translated(0, -1),
                          juce::Justification::centred, false);
    }
};

struct AssistantActionButton : public juce::TextButton
{
    using juce::TextButton::TextButton;

    void paintButton(juce::Graphics &graphics, bool isMouseOverButton, bool isButtonDown) override
    {
        auto highlighted = isMouseOverButton || isButtonDown;
        auto background = findColour(highlighted ? buttonOnColourId : buttonColourId);
        auto foreground = findColour(highlighted ? textColourOnId : textColourOffId);
        if (!isEnabled())
        {
            background = background.withMultipliedAlpha(0.5f);
            foreground = foreground.withMultipliedAlpha(0.5f);
        }
        else if (isButtonDown)
        {
            background = background.darker(0.1f);
        }

        graphics.setColour(background);
        graphics.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        graphics.setColour(foreground);
        graphics.setFont(juce::Font(11.5f, juce::Font::bold));
        graphics.drawFittedText(getButtonText(), getLocalBounds().reduced(4, 2),
                                juce::Justification::centred, 2, 0.9f);
    }
};

class AssistantConnectionOverlay : public juce::Component
{
  public:
    explicit AssistantConnectionOverlay(Surge::Assistant::Provider provider)
        : provider(provider), apiKey("Assistant API key"), title("Assistant connection", {}),
          instructions("Assistant connection instructions", {}),
          status("Assistant connection status", {}), connectButton("Connect"),
          cancelButton("Cancel"), keyPageButton("Get API key")
    {
        setWantsKeyboardFocus(true);
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);

        title.setText("Connect " + Surge::Assistant::providerDisplayName(provider),
                      juce::dontSendNotification);
        title.setFont(juce::Font(20.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        juce::String keyStorageInstructions;
#if JUCE_MAC || JUCE_WINDOWS
        keyStorageInstructions =
            "Enter the API or subscription key from your provider account. It will be stored in "
            "the operating system credential store, never in a patch or DAW project.";
#else
        keyStorageInstructions =
            "Enter the API or subscription key from your provider account. It is kept in memory "
            "for this Surge XT session only, never in a patch or DAW project.";
#endif
        instructions.setText(
            provider == Surge::Assistant::Provider::ChatGPT
                ? "Use the official Codex runtime to sign in with ChatGPT in your browser. Surge "
                  "XT never receives or stores your ChatGPT tokens."
                : keyStorageInstructions,
            juce::dontSendNotification);
        instructions.setFont(juce::Font(13.0f));
        instructions.setColour(juce::Label::textColourId, juce::Colour(205, 205, 205));
        instructions.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(instructions);

        apiKey.setMultiLine(false);
        apiKey.setInputRestrictions(4096);
        apiKey.setWantsKeyboardFocus(true);
        apiKey.setPasswordCharacter('*');
        apiKey.setTextToShowWhenEmpty("Paste API key", juce::Colour(155, 155, 155));
        apiKey.setColour(juce::TextEditor::backgroundColourId, juce::Colour(23, 23, 23));
        apiKey.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        apiKey.setColour(juce::TextEditor::outlineColourId, juce::Colour(95, 95, 95));
        apiKey.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(240, 160, 35));
        apiKey.setSelectAllWhenFocused(true);
        Surge::Widgets::fixupJuceTextEditorAccessibility(apiKey);
        apiKey.onTextChange = [this]() {
            connectButton.setEnabled(apiKey.getText().trim().isNotEmpty());
        };
        apiKey.onReturnKey = [this]() {
            if (connectButton.isEnabled())
                connectButton.triggerClick();
        };
        apiKey.onEscapeKey = [this]() {
            if (cancelButton.isEnabled())
                cancelButton.triggerClick();
        };
        addAndMakeVisible(apiKey);

        status.setFont(juce::Font(12.0f));
        status.setColour(juce::Label::textColourId, juce::Colour(230, 180, 80));
        status.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status);

        auto configureButton = [](juce::TextButton &button, juce::Colour colour) {
            button.setColour(juce::TextButton::buttonColourId, colour);
            button.setColour(juce::TextButton::buttonOnColourId, colour.brighter(0.12f));
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        };
        configureButton(connectButton, juce::Colour(180, 105, 20));
        configureButton(cancelButton, juce::Colour(65, 65, 65));
        configureButton(keyPageButton, juce::Colour(55, 70, 85));
        if (provider == Surge::Assistant::Provider::ChatGPT)
        {
            apiKey.setVisible(false);
            connectButton.setButtonText("Sign in");
            connectButton.setEnabled(true);
            keyPageButton.setButtonText("Install Codex");
            status.setText("An existing ChatGPT login will be reused automatically.",
                           juce::dontSendNotification);
        }
        else
        {
            connectButton.setEnabled(false);
        }
        connectButton.onClick = [this]() {
            auto key = apiKey.getText().trim();
            apiKey.clear();
            if (onConnect)
                onConnect(key);
        };
        cancelButton.onClick = [this]() {
            if (onCancel)
                onCancel();
        };
        keyPageButton.onClick = [this]() {
            auto page = Surge::Assistant::providerKeyPage(this->provider);
            if (page.isNotEmpty())
                juce::URL(page).launchInDefaultBrowser();
        };
        addAndMakeVisible(connectButton);
        addAndMakeVisible(cancelButton);
        addAndMakeVisible(keyPageButton);
    }

    void paint(juce::Graphics &graphics) override
    {
        graphics.fillAll(juce::Colours::black.withAlpha(0.72f));
        auto panel = panelBounds().toFloat();
        graphics.setColour(juce::Colour(35, 35, 35));
        graphics.fillRoundedRectangle(panel, 7.0f);
        graphics.setColour(juce::Colour(238, 155, 30));
        graphics.drawRoundedRectangle(panel, 7.0f, 1.5f);
    }

    void resized() override
    {
        auto content = panelBounds().reduced(20);
        title.setBounds(content.removeFromTop(30));
        content.removeFromTop(4);
        instructions.setBounds(content.removeFromTop(46));
        content.removeFromTop(8);
        auto keyRow = content.removeFromTop(32);
        if (provider == Surge::Assistant::Provider::ChatGPT)
        {
            keyPageButton.setBounds(keyRow.removeFromLeft(124));
        }
        else
        {
            keyPageButton.setBounds(keyRow.removeFromRight(104));
            keyRow.removeFromRight(8);
            apiKey.setBounds(keyRow);
        }
        content.removeFromTop(4);
        status.setBounds(content.removeFromTop(24));
        auto buttons = content.removeFromBottom(30);
        cancelButton.setBounds(buttons.removeFromRight(96));
        buttons.removeFromRight(8);
        connectButton.setBounds(buttons.removeFromRight(112));
    }

    void setBusy(bool busy)
    {
        connectButton.setEnabled(!busy && (provider == Surge::Assistant::Provider::ChatGPT ||
                                           apiKey.getText().trim().isNotEmpty()));
        cancelButton.setButtonText(busy ? "Cancel request" : "Cancel");
        cancelButton.setEnabled(true);
        keyPageButton.setEnabled(!busy);
        if (busy)
            status.setText("Testing connection...", juce::dontSendNotification);
    }

    void setStatus(const juce::String &message)
    {
        status.setText(message, juce::dontSendNotification);
    }

    void setCancelling()
    {
        connectButton.setEnabled(false);
        cancelButton.setButtonText("Cancelling...");
        cancelButton.setEnabled(false);
        keyPageButton.setEnabled(false);
        status.setText("Cancelling connection...", juce::dontSendNotification);
    }

    AssistantPromptEditor &keyEditor() { return apiKey; }
    Surge::Assistant::Provider getProvider() const { return provider; }
    void focusInitialControl()
    {
        if (provider == Surge::Assistant::Provider::ChatGPT)
            Surge::GUI::grabKeyboardFocusIfAllowed(&connectButton);
        else
            Surge::GUI::grabKeyboardFocusIfAllowed(&apiKey);
    }

    std::function<void(const juce::String &)> onConnect;
    std::function<void()> onCancel;

  private:
    juce::Rectangle<int> panelBounds() const
    {
        constexpr int panelWidth = 540;
        constexpr int panelHeight = 220;
        return juce::Rectangle<int>(panelWidth, panelHeight)
            .withCentre(getLocalBounds().getCentre());
    }

    Surge::Assistant::Provider provider;
    AssistantPromptEditor apiKey;
    juce::Label title;
    juce::Label instructions;
    juce::Label status;
    juce::TextButton connectButton;
    juce::TextButton cancelButton;
    juce::TextButton keyPageButton;
};

struct VKeyboardWheel : public juce::Component
{
    std::function<void(int)> onValueChanged = [](int f) {};
    bool snapBack{false};
    bool unipolar{true};
    int range{127};
    int value{0};
    void paint(juce::Graphics &g) override
    {
        auto wheelSz = getLocalBounds().reduced(2, 3);

        g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelBgId));
        g.fillRect(wheelSz);
        g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelBorderId));
        g.drawRect(wheelSz.expanded(1, 1));

        float p = 1.0 * value / range;

        if (!unipolar)
        {
            p = 1.0 * (value + range) / (2 * range);
        }

        // y direction is flipped
        p = 1 - p;

        float cp = wheelSz.getY() + p * (wheelSz.getHeight() - 4);
        auto r = wheelSz.withHeight(2).translated(0, cp - 2).reduced(1, 0);

        g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelValueId));
        g.fillRect(r);
    }

    void valueFromY(float y)
    {
        auto wheelSz = getLocalBounds().reduced(1, 2);
        auto py = std::clamp(y, 1.f * wheelSz.getY(), 1.f * wheelSz.getY() + wheelSz.getHeight());
        py = (py - wheelSz.getY()) / wheelSz.getHeight();
        py = 1 - py;

        if (unipolar)
            value = py * range;
        else
            value = 2 * py * range - range;
        onValueChanged(value);
    }

    void mouseDown(const juce::MouseEvent &event) override
    {
        valueFromY(event.position.y);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent &event) override
    {
        valueFromY(event.position.y);
        repaint();
    }

    void mouseUp(const juce::MouseEvent &event) override
    {
        if (snapBack)
        {
            value = 0;
            onValueChanged(value);
        }
        repaint();
    }
};

struct VKeyboardSus : public juce::Component
{
    std::function<void(int)> onValueChanged = [](int f) {};
    bool isOn{false};

    void paint(juce::Graphics &g) override
    {
        auto wheelSz = getLocalBounds().reduced(1, 2);

        g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelBgId));
        g.fillRect(wheelSz);

        if (isOn)
        {
            g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelValueId));
            g.fillRect(wheelSz.reduced(1, 1));
        }

        g.setColour(findColour(SurgeJUCELookAndFeel::SurgeColourIds::wheelBorderId));
        g.drawRect(wheelSz.expanded(1, 1));
    }

    void mouseDown(const juce::MouseEvent &event) override
    {
        isOn = !isOn;
        onValueChanged(isOn * 127);
        repaint();
    }
};

static std::weak_ptr<SurgeJUCELookAndFeel> surgeLookAndFeelWeakPointer;
static std::mutex surgeLookAndFeelSetupMutex;

//==============================================================================

SurgeVirtualKeyboard::~SurgeVirtualKeyboard() { clearAllLatches(); }

float SurgeVirtualKeyboard::getCurrentVelocity() const { return editor->midiKeyboardVelocity; }

//==============================================================================

SurgeSynthEditor::SurgeSynthEditor(SurgeSynthProcessor &p)
    : juce::AudioProcessorEditor(&p), processor(p)
{
    // Marks this editor as the popup-scale anchor for the SURGE PATCH in
    // juce_TextEditor.cpp so TextEditor right-click menus inherit only the
    // host scale factor and not the UI zoom transform applied to frame.
    getProperties().set("SSTPopupAnchor", true);

    {
        std::lock_guard<std::mutex> grd(surgeLookAndFeelSetupMutex);
        if (auto sp = surgeLookAndFeelWeakPointer.lock())
        {
            surgeLF = sp;
        }
        else
        {
            surgeLF = std::make_shared<SurgeJUCELookAndFeel>();
            surgeLookAndFeelWeakPointer = surgeLF;

            juce::LookAndFeel::setDefaultLookAndFeel(surgeLF.get());
        }

        surgeLF->addStorage(&(processor.surge->storage));
    }

    addKeyListener(this);

    topLevelContainer = std::make_unique<juce::Component>();
    addAndMakeVisible(*topLevelContainer);

    sge = std::make_unique<SurgeGUIEditor>(this, processor.surge.get());
    assistantClient = std::make_unique<Surge::Assistant::Client>();

    for (auto provider : Surge::Assistant::availableProviders())
    {
        auto defaultModel = Surge::Assistant::providerDefaultModel(provider);
        if (defaultModel.isNotEmpty())
            assistantModels[provider] = {defaultModel};
    }

    assistantProvider = Surge::Assistant::providerFromId(Surge::Storage::getUserDefaultValue(
        &processor.surge->storage, Surge::Storage::AssistantProvider, "none"));
    assistantModel = Surge::Storage::getUserDefaultValue(
        &processor.surge->storage, Surge::Storage::AssistantModel,
        Surge::Assistant::providerDefaultModel(assistantProvider).toStdString());
    if (assistantProvider != Surge::Assistant::Provider::None && assistantModel.isEmpty())
        assistantModel = Surge::Assistant::providerDefaultModel(assistantProvider);

    assistantButton = std::make_unique<AssistantActionButton>("Ask\nAssistant");
    addAndMakeVisible(*assistantButton);
    assistantButton->setColour(juce::TextButton::buttonColourId, juce::Colour(238, 155, 30));
    assistantButton->setColour(juce::TextButton::buttonOnColourId, juce::Colour(255, 185, 60));
    assistantButton->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    assistantButton->setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    assistantButton->setMouseClickGrabsKeyboardFocus(false);
    assistantButton->setTitle("Ask Assistant");
    assistantButton->setDescription("Ask the assistant to update the current sound.");
    assistantButton->onClick = [this]() {
        if (assistantPendingAction == AssistantPendingAction::Generate)
            cancelAssistantRequest();
        else
            submitAssistantPrompt();
    };

    assistantClearPatchButton = std::make_unique<AssistantActionButton>("Clear\nPatch");
    assistantClearPatchButton->setColour(juce::TextButton::buttonColourId,
                                         juce::Colour(0x12, 0x34, 0x63));
    assistantClearPatchButton->setColour(juce::TextButton::buttonOnColourId,
                                         juce::Colour(0x23, 0x64, 0xC0));
    assistantClearPatchButton->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    assistantClearPatchButton->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    assistantClearPatchButton->setMouseClickGrabsKeyboardFocus(false);
    assistantClearPatchButton->setTitle("Clear Patch");
    assistantClearPatchButton->setDescription(
        "Reset the current patch to the Surge XT default without contacting the assistant.");
    assistantClearPatchButton->onClick = [this]() { requestClearPatch(); };
    addAndMakeVisible(*assistantClearPatchButton);

    assistantConnectionButton = std::make_unique<AssistantGearButton>();
    assistantConnectionButton->setColour(juce::TextButton::buttonColourId,
                                         juce::Colour(48, 55, 62));
    assistantConnectionButton->setColour(juce::TextButton::buttonOnColourId,
                                         juce::Colour(65, 75, 85));
    assistantConnectionButton->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    assistantConnectionButton->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    assistantConnectionButton->setMouseClickGrabsKeyboardFocus(false);
    assistantConnectionButton->onClick = [this]() { showAssistantConnectionMenu(); };
    addAndMakeVisible(*assistantConnectionButton);
    updateAssistantConnectionButton();

    auto promptEditor = std::make_unique<AssistantPromptEditor>("Assistant prompt");
    promptEditor->onFocusChanged = [this](bool hasFocus) { setAssistantPromptFocus(hasFocus); };
    assistantPrompt = std::move(promptEditor);
    assistantPrompt->setMultiLine(false);
    assistantPrompt->setInputRestrictions(2000);
    assistantPrompt->setWantsKeyboardFocus(true);
    assistantPrompt->setMouseClickGrabsKeyboardFocus(true);
    assistantPrompt->setFont(juce::Font(16.0f));
    assistantPrompt->setJustification(juce::Justification::centredLeft);
    assistantPrompt->setColour(juce::TextEditor::backgroundColourId, juce::Colour(28, 28, 28));
    assistantPrompt->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    assistantPrompt->setColour(juce::TextEditor::outlineColourId, juce::Colour(90, 90, 90));
    assistantPrompt->setColour(juce::TextEditor::focusedOutlineColourId,
                               juce::Colour(240, 160, 35));
    assistantPrompt->setTextToShowWhenEmpty(
        "Describe a sound or ask for a change to the current patch...",
        juce::Colour(180, 180, 180));
    Surge::Widgets::fixupJuceTextEditorAccessibility(*assistantPrompt);
    assistantPrompt->onReturnKey = [this]() { submitAssistantPrompt(); };
    addAndMakeVisible(*assistantPrompt);

    assistantStatus = std::make_unique<juce::Label>("Assistant status", "Assistant ready");
    assistantStatus->setJustificationType(juce::Justification::topLeft);
    assistantStatus->setColour(juce::Label::textColourId, juce::Colours::white);
    assistantStatus->setColour(juce::Label::backgroundColourId, juce::Colour(24, 24, 24));
    assistantStatus->setOpaque(true);
    assistantStatus->setFont(juce::Font(12.0f));
    assistantStatus->setText(
        assistantProvider == Surge::Assistant::Provider::None
            ? "Local fallback: try \"casio retro keyboard sound\", then \"a bit more reverb\"."
            : "Ready to create or update the current patch with " +
                  Surge::Assistant::providerDisplayName(assistantProvider) + ".",
        juce::dontSendNotification);
    addAndMakeVisible(*assistantStatus);

    auto mcValue = Surge::Storage::getUserDefaultValue(&(this->processor.surge->storage),
                                                       Surge::Storage::MiddleC, 1);

    keyboard = std::make_unique<SurgeVirtualKeyboard>(
        this, processor.midiKeyboardState,
        juce::MidiKeyboardComponent::Orientation::horizontalKeyboard);
    keyboard->setVelocity(midiKeyboardVelocity, true);
    keyboard->setOctaveForMiddleC(5 - mcValue);
    keyboard->setKeyPressBaseOctave(midiKeyboardOctave);
    keyboard->setLowestVisibleKey(24);
    // this makes VKB always receive keyboard input (except when we focus on any typeins, of course)
    keyboard->setWantsKeyboardFocus(false);

    // Load the saved VKB click velocity mode setting
    bool virtualKeyboardClickSetsVelocity = Surge::Storage::getUserDefaultValue(
        &(this->processor.surge->storage), Surge::Storage::VirtualKeyboardClickSetsVelocity, false);
    if (auto vkb = dynamic_cast<SurgeVirtualKeyboard *>(keyboard.get()))
    {
        vkb->useClickPositionForOverallVelocity = virtualKeyboardClickSetsVelocity;
        // Set up callback to update midiKeyboardVelocity when mode is enabled
        vkb->onVelocityChanged = [this](float vel) { midiKeyboardVelocity = vel; };
    }

    auto vkbLayout = Surge::Storage::getUserDefaultValue(
        &(this->processor.surge->storage), Surge::Storage::VirtualKeyboardLayout, "QWERTY");
    setVKBLayout(vkbLayout);

    auto w = std::make_unique<VKeyboardWheel>();
    w->snapBack = true;
    w->unipolar = false;
    w->range = 8196;
    w->onValueChanged = [this](auto f) {
        processor.midiFromGUI.push(
            SurgeSynthProcessor::midiR(SurgeSynthProcessor::midiR::PITCHWHEEL, f));
    };
    pitchwheel = std::move(w);

    auto m = std::make_unique<VKeyboardWheel>();
    m->onValueChanged = [this](auto f) {
        processor.midiFromGUI.push(
            SurgeSynthProcessor::midiR(SurgeSynthProcessor::midiR::MODWHEEL, f));
    };
    modwheel = std::move(m);

    auto sp = std::make_unique<VKeyboardSus>();
    sp->onValueChanged = [this](auto f) {
        processor.midiFromGUI.push(
            SurgeSynthProcessor::midiR(SurgeSynthProcessor::midiR::SUSPEDAL, f));
    };
    suspedal = std::move(sp);

    tempoTypein = std::make_unique<juce::TextEditor>("Tempo");
    tempoTypein->setFont(sge->currentSkin->fontManager->getLatoAtSize(9));
    tempoTypein->setInputRestrictions(3, "0123456789");
    tempoTypein->setSelectAllWhenFocused(true);
    tempoTypein->onReturnKey = [this]() {
        // this is thread sloppy
        float newT = std::atof(tempoTypein->getText().toRawUTF8());

        processor.standaloneTempo = newT;
        processor.surge->storage.unstreamedTempo = newT;
        tempoTypein->giveAwayKeyboardFocus();
    };

    tempoLabel = std::make_unique<juce::Label>("Tempo", "Tempo");
    sustainLabel = std::make_unique<juce::Label>("Sustain", "Sustain");

    topLevelContainer->addChildComponent(*keyboard);
    topLevelContainer->addChildComponent(*pitchwheel);
    topLevelContainer->addChildComponent(*modwheel);
    topLevelContainer->addChildComponent(*suspedal);
    topLevelContainer->addChildComponent(*tempoLabel);
    topLevelContainer->addChildComponent(*sustainLabel);
    topLevelContainer->addChildComponent(*tempoTypein);

    drawExtendedControls = sge->getShowVirtualKeyboard();

    bool addTempo = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    int yExtra = 0;

    if (drawExtendedControls)
    {
        keyboard->setVisible(true);
        tempoLabel->setVisible(addTempo);
        tempoTypein->setVisible(addTempo);

        yExtra = extraYSpaceForVirtualKeyboard;
    }
    else
    {
        keyboard->setVisible(false);
        tempoLabel->setVisible(false);
        tempoTypein->setVisible(false);

        yExtra = 0;
    }

    auto rg = BlockRezoom(this);
    setSize(BASE_WINDOW_SIZE_X,
            BASE_WINDOW_SIZE_Y + yExtra + assistantBarHeight + assistantResponseHeight);
    // add the bottom right corner resizer only for VST2
    setResizable(true, processor.wrapperType == juce::AudioProcessor::wrapperType_VST);

    sge->audioLatencyNotified = processor.inputIsLatent;
    if (juce::Desktop::getInstance().isHeadless() == false)
        sge->open(nullptr);

    // The frame does not exist during the first resized() call. Shift it after open so
    // the assistant rows occupy real space instead of covering Surge's header.
    sge->moveTopLeftTo(0, 0);

    // Surge's main frame is created by open(); keep the assistant controls above it.
    assistantPrompt->toFront(false);
    assistantStatus->toFront(false);
    assistantConnectionButton->toFront(false);
    assistantClearPatchButton->toFront(false);
    assistantButton->toFront(false);

    idleTimer = std::make_unique<IdleTimer>(this);
    idleTimer->startTimer(1000 / 60);
}

SurgeSynthEditor::~SurgeSynthEditor()
{
    idleTimer->stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();

    if (assistantClient)
    {
        if (assistantPendingAction != AssistantPendingAction::None && assistantFuture.valid())
        {
            assistantCancellationRequested =
                assistantCancellationRequested || assistantClient->cancel();
            assistantFuture.wait();
            pollAssistantResult();
        }
        assistantClient->cancel();
        assistantClient.reset();
    }
    assistantConnectionOverlay.reset();

    setAssistantPromptFocus(false);
    assistantPrompt.reset();

    sge->close();

    if (sge->bitmapStore)
    {
        sge->bitmapStore->clearAllLoadedBitmaps();
    }

    surgeLF->removeStorage(&(processor.surge->storage));

    sge.reset(nullptr);
}

void SurgeSynthEditor::setVKBLayout(const std::string layout)
{
    auto searchTerm = [&layout](const auto &x) { return x.first == layout; };
    auto search = std::find_if(vkbLayouts.begin(), vkbLayouts.end(), searchTerm);

    if (search != vkbLayouts.end())
    {
        keyboard->clearKeyMappings();

        unsigned int n = 0;

        for (auto i : search->second)
        {
            // Don't bind accessible action keys to the keyboard
            if (Surge::GUI::allowKeyboardEdits(&processor.surge->storage))
            {
                // Don't know why we have high bit set on the keys? Do both to be sure
                auto b1 = Surge::Widgets::isAccessibleKey((juce::KeyPress)i);
                auto b2 =
                    (i > 128) ? Surge::Widgets::isAccessibleKey((juce::KeyPress)(i - 128)) : false;

                if (b1 || b2)
                {
                    continue;
                }
            }
#if JUCE_LINUX
            // See issue #7604
            if (i < 128)
            {
                keyboard->setKeyPressForNote((juce::KeyPress)i, n);
            }
#else
            keyboard->setKeyPressForNote((juce::KeyPress)i, n);
#endif

            n++;
        }
    }
}

void SurgeSynthEditor::handleAsyncUpdate() {}

void SurgeSynthEditor::paint(juce::Graphics &g)
{
    g.fillAll(findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoBackgroundId));

#if DEBUG_FULLSCREENBOUNDS
    /* For debugging fullscreen */
    g.setColour(juce::Colours::red);
    for (int x = 100; x < getWidth(); x += 100)
    {
        g.drawLine(x, 0, x, getHeight(), 1);
    }
    for (int y = 100; y < getWidth(); y += 100)
    {
        g.drawLine(0, y, getWidth(), y, 1);
    }
#endif
}

void SurgeSynthEditor::idle()
{
    pollAssistantResult();

    if (assistantClearPatchConfirmationPending && (!sge->alert || !sge->alert->isVisible()))
    {
        assistantClearPatchConfirmationPending = false;
        setAssistantWorking(false);
    }

    if (assistantClearPatchPending || assistantPatchPending)
    {
        processor.surge->processAudioThreadOpsWhenAudioEngineUnavailable();
        auto completedSequence = processor.surge->completedRawLoadSequence.load();
        if (completedSequence > assistantPatchLoadSequence)
        {
            assistantClearPatchPending = false;
            assistantPatchPending = false;
            assistantPatchLoadSequence = 0;
            setAssistantWorking(false);
            assistantStatus->setText(
                "The patch changed while it was being cleared, so the reset was cancelled.",
                juce::dontSendNotification);
        }
        else if (completedSequence == assistantPatchLoadSequence &&
                 !processor.surge->rawLoadEnqueued.load())
        {
            assistantPatchLoadSequence = 0;
            if (assistantPatchPending)
            {
                assistantPatchPending = false;
                setAssistantWorking(false);
                applyCasioRetroKeyboardPatch();
            }
            else
            {
                assistantClearPatchPending = false;
                setAssistantWorking(false);
                assistantStatus->setText(
                    "Patch cleared and reset to default. Type or edit a description, then choose "
                    "Ask Assistant when ready.",
                    juce::dontSendNotification);
            }
        }
    }

    sge->idle();

    if (processor.surge->refresh_vkb)
    {
        const int curTypeinBPM = std::atoi(tempoTypein->getText().toStdString().c_str());
        const int curBPM = std::round(processor.surge->time_data.tempo);

        if (curTypeinBPM != curBPM)
        {
            tempoTypein->setText(fmt::format("{}", curBPM));
        }

        processor.surge->refresh_vkb = false;
    }
}

void SurgeSynthEditor::reapplySurgeComponentColours()
{
    tempoLabel->setColour(juce::Label::textColourId,
                          findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoLabelId));
    sustainLabel->setColour(juce::Label::textColourId,
                            findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoLabelId));

    tempoTypein->setColour(
        juce::TextEditor::backgroundColourId,
        findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinBackgroundId));
    tempoTypein->setColour(juce::TextEditor::outlineColourId,
                           findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinBorderId));
    tempoTypein->setColour(juce::TextEditor::focusedOutlineColourId,
                           findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinBorderId));
    tempoTypein->setColour(
        juce::TextEditor::highlightColourId,
        findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinHighlightId));
    tempoTypein->setColour(juce::TextEditor::highlightedTextColourId,
                           findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinTextId));
    tempoTypein->setColour(juce::TextEditor::textColourId,
                           findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinTextId));
    tempoTypein->applyColourToAllText(
        findColour(SurgeJUCELookAndFeel::SurgeColourIds::tempoTypeinTextId), true);

    for (auto *p = getParentComponent(); p != nullptr; p = p->getParentComponent())
    {
        if (auto dw = dynamic_cast<juce::DocumentWindow *>(p))
        {
            dw->setName("Surge XT");

            if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
            {
                dw->setColour(juce::DocumentWindow::backgroundColourId,
                              findColour(SurgeJUCELookAndFeel::SurgeColourIds::topWindowBorderId));
            }
        }
    }

    repaint();
}

void SurgeSynthEditor::resized()
{
    auto assistantOffset = assistantBarHeight + assistantResponseHeight;
    topLevelContainer->setBounds(getLocalBounds().withTop(assistantOffset));
    drawExtendedControls = sge->getShowVirtualKeyboard();

    auto w = getWidth();
    auto h =
        getHeight() - assistantBarHeight - assistantResponseHeight -
        (drawExtendedControls ? 0.01 * sge->getZoomFactor() * extraYSpaceForVirtualKeyboard : 0);

    auto assistantBar = getLocalBounds().withHeight(assistantBarHeight).reduced(8, 6);
    assistantConnectionButton->setBounds(assistantBar.removeFromRight(40));
    assistantBar.removeFromRight(8);
    auto assistantActions = assistantBar.removeFromRight(150);
    assistantClearPatchButton->setBounds(assistantActions.removeFromRight(74));
    assistantActions.removeFromRight(2);
    assistantButton->setBounds(assistantActions);
    assistantBar.removeFromRight(8);
    assistantPrompt->setBounds(assistantBar);
    assistantPrompt->setIndents(
        8, std::max(0, (assistantPrompt->getHeight() - assistantPrompt->getTextHeight()) / 2));
    assistantStatus->setBounds(getLocalBounds()
                                   .withTop(assistantBarHeight)
                                   .withHeight(assistantResponseHeight)
                                   .reduced(8, 2));
    if (assistantConnectionOverlay)
    {
        assistantConnectionOverlay->setBounds(getLocalBounds());
        assistantConnectionOverlay->toFront(false);
    }
    if (Surge::GUI::getIsStandalone())
    {
        juce::Component *comp = this;
        while (comp)
        {
            auto *cdw = dynamic_cast<juce::ResizableWindow *>(comp);
            if (cdw)
            {
                if (cdw->isFullScreen())
                {
                    auto b = getLocalBounds();
                    auto xw = 1.f * sge->getWindowSizeX() / b.getWidth();
                    auto xh = 1.f *
                              (sge->getWindowSizeY() +
                               (drawExtendedControls ? extraYSpaceForVirtualKeyboard : 0) +
                               assistantBarHeight + assistantResponseHeight) /
                              b.getHeight();

                    auto nz = std::min(1.0 / xw, 1.0 / xh);
                    auto snz = nz / sge->getZoomFactor() * 100.f;

                    topLevelContainer->setTransform(juce::AffineTransform().scaled(snz));

                    // target width
                    auto tw = sge->getWindowSizeX() * sge->getZoomFactor() * 0.01 * snz;
                    auto th = (sge->getWindowSizeY() +
                               (drawExtendedControls ? extraYSpaceForVirtualKeyboard : 0)) *
                              sge->getZoomFactor() * 0.01 * snz;

                    auto pw = (b.getWidth() - tw) / 2.0;
                    auto ph = (b.getHeight() - th) / 2.0;

                    // turn off aspect ratio
                    if (getConstrainer())
                        getConstrainer()->setFixedAspectRatio(0.f);

                    sge->moveTopLeftTo(std::round(pw / snz), std::round(ph / snz));
                    return;
                }
                comp = nullptr;
            }
            else
            {
                comp = comp->getParentComponent();
            }
        }

        sge->moveTopLeftTo(0, 0);
    }

    topLevelContainer->setTransform(juce::AffineTransform());

    auto b = getLocalBounds();
    auto wR = 1.0 * w / sge->getWindowSizeX();
    auto hR = 1.0 * h / sge->getWindowSizeY();

    auto zoom = 0.01f * sge->getZoomFactor();
    auto ar = zoom * sge->getWindowSizeX() /
              (zoom * (sge->getWindowSizeY() +
                       (drawExtendedControls ? extraYSpaceForVirtualKeyboard : 0)) +
               assistantOffset);
    if (getConstrainer())
        getConstrainer()->setFixedAspectRatio(ar);

    auto zfn = std::min(wR, hR);
    if (wR < 1 && hR < 1)
        zfn = std::max(wR, hR);
    if ((wR - 1) * (hR - 1) < 0)
        zfn = std::min(zfn, 1.0);

    zfn = 100.0 * zfn / sge->getZoomFactor();

    float applyZoomFactor = sge->getZoomFactor() * 0.01;
    if (!rezoomGuard)
        applyZoomFactor *= zfn;

    bool addTempo = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;

    if (drawExtendedControls)
    {
        auto y = sge->getWindowSizeY();
        auto x = addTempo ? 50 : 0;
        auto wheels = 32;
        auto margin = 6;
        int noTempoSusYOffset = -16;
        int tempoHeight = 10, typeinHeight = 14;
        int tempoBlockHeight = tempoHeight + typeinHeight;

        auto xf = juce::AffineTransform().scaled(applyZoomFactor);
        auto r = juce::Rectangle<int>(x + wheels + margin, y,
                                      sge->getWindowSizeX() - x - wheels - margin,
                                      extraYSpaceForVirtualKeyboard);

        keyboard->setBounds(r);
        keyboard->setTransform(xf);
        keyboard->setVisible(true);

        auto pmr = juce::Rectangle<int>(x, y, wheels / 2, extraYSpaceForVirtualKeyboard);
        pitchwheel->setBounds(pmr);
        pitchwheel->setTransform(xf);
        pitchwheel->setVisible(true);
        pmr = pmr.translated((wheels / 2) + margin / 3, 0);
        modwheel->setBounds(pmr);
        modwheel->setTransform(xf);
        modwheel->setVisible(true);

        if (addTempo)
        {
            tempoLabel->setBounds(4, y, x - 8, tempoHeight);
            tempoLabel->setFont(sge->currentSkin->fontManager->getLatoAtSize(8, juce::Font::bold));
            tempoLabel->setJustificationType(juce::Justification::centred);
            tempoLabel->setTransform(xf);
            tempoLabel->setVisible(addTempo);

            tempoTypein->setBounds(4, y + tempoHeight, x - 8, typeinHeight);
            tempoTypein->setText(
                std::to_string((int)(processor.surge->storage.temposyncratio * 120)));
            tempoTypein->setFont(sge->currentSkin->fontManager->getLatoAtSize(9));
            tempoTypein->setIndents(4, -1);
            tempoTypein->setJustification(juce::Justification::centred);
            tempoTypein->setTransform(xf);
            tempoTypein->setVisible(addTempo);
        }

        auto sml = juce::Rectangle<int>(4, y + tempoBlockHeight, x - 8, tempoHeight);
        sml.translate(0, addTempo ? 0 : noTempoSusYOffset);
        sustainLabel->setBounds(sml);
        sustainLabel->setFont(sge->currentSkin->fontManager->getLatoAtSize(8, juce::Font::bold));
        sustainLabel->setJustificationType(juce::Justification::centred);
        sustainLabel->setTransform(xf);
        sustainLabel->setVisible(true);

        auto smr =
            juce::Rectangle<int>(4, y + tempoBlockHeight + tempoHeight, x - 8, typeinHeight / 2);
        smr = smr.withBottom(pmr.getBottom() - 1).translated(0, addTempo ? 0 : noTempoSusYOffset);
        suspedal->setBounds(smr);
        suspedal->setTransform(xf);
        suspedal->setVisible(true);
    }
    else
    {
        keyboard->setVisible(false);
        tempoLabel->setVisible(false);
        tempoTypein->setVisible(false);
    }

    if (zfn != 1.0 && rezoomGuard == 0)
    {
        auto br = BlockRezoom(this);
        sge->setZoomFactor(round(sge->getZoomFactor() * zfn), false);
    }
}

void SurgeSynthEditor::submitAssistantPrompt()
{
    if (assistantPatchPending || assistantClearPatchPending ||
        assistantClearPatchConfirmationPending ||
        assistantPendingAction != AssistantPendingAction::None)
    {
        assistantStatus->setText("Another assistant action or patch reset is already in progress.",
                                 juce::dontSendNotification);
        return;
    }

    auto prompt = assistantPrompt->getText().trim();
    if (prompt.isEmpty())
    {
        assistantStatus->setText("Enter a sound description first.", juce::dontSendNotification);
        return;
    }

    if (assistantProvider != Surge::Assistant::Provider::None)
    {
        assistantRequestWasFresh = isCurrentPatchUntouchedInit();
        assistantRequestPrompt = prompt;
        auto request = buildAssistantPatchRequest(prompt, assistantRequestWasFresh);
        if (request.parameters.empty())
        {
            assistantStatus->setText("No editable patch parameters are available.",
                                     juce::dontSendNotification);
            return;
        }

        assistantCancellationRequested = false;
        assistantFuture = assistantClient->generate(std::move(request));
        assistantPendingAction = AssistantPendingAction::Generate;
        assistantGenerationPending = true;
        setAssistantWorking(true);
        assistantStatus->setText(
            "Asking " + Surge::Assistant::providerDisplayName(assistantProvider) + " / " +
                Surge::Assistant::modelDisplayName(assistantProvider, assistantModel) +
                (assistantRequestWasFresh ? " to create a patch..."
                                          : " for a restrained update..."),
            juce::dontSendNotification);
        return;
    }

    auto request = prompt.toLowerCase();
    if (!isCurrentPatchUntouchedInit())
    {
        if (request.contains("reverb"))
        {
            applyMoreReverb();
        }
        else
        {
            assistantStatus->setText(
                "Follow-up prototype: try \"a bit more reverb\". The current patch is preserved.",
                juce::dontSendNotification);
        }
        return;
    }

    auto isCasioRequest =
        request.contains("casio") || (request.contains("retro") && request.contains("keyboard"));
    if (!isCasioRequest)
    {
        assistantStatus->setText("Fresh-patch prototype: try \"casio retro keyboard sound\".",
                                 juce::dontSendNotification);
        return;
    }

    if (!enqueueDefaultPatch())
        return;

    assistantPatchPending = true;
    setAssistantWorking(true);
    assistantStatus->setText("Creating a Casio-style retro keyboard patch...",
                             juce::dontSendNotification);
}

void SurgeSynthEditor::requestClearPatch()
{
    if (assistantPatchPending || assistantClearPatchPending ||
        assistantClearPatchConfirmationPending ||
        assistantPendingAction != AssistantPendingAction::None)
    {
        assistantStatus->setText("Another assistant action or patch reset is already in progress.",
                                 juce::dontSendNotification);
        return;
    }

    assistantClearPatchConfirmationPending = true;
    assistantButton->setEnabled(false);
    assistantClearPatchButton->setEnabled(false);
    assistantConnectionButton->setEnabled(false);

    auto safeThis = juce::Component::SafePointer<SurgeSynthEditor>(this);
    sge->alertYesNo(
        "Clear Patch?",
        "This will reset every setting in the current patch to the Surge XT default. No "
        "assistant request will be sent, and you can undo the reset afterward. Are you sure?",
        [safeThis]() {
            if (safeThis == nullptr)
                return;
            safeThis->assistantClearPatchConfirmationPending = false;
            safeThis->beginClearPatch();
        },
        [safeThis]() {
            if (safeThis == nullptr)
                return;
            safeThis->assistantClearPatchConfirmationPending = false;
            safeThis->setAssistantWorking(false);
        });
}

void SurgeSynthEditor::beginClearPatch()
{
    if (assistantPatchPending || assistantClearPatchPending ||
        assistantPendingAction != AssistantPendingAction::None)
        return;

    if (!enqueueDefaultPatch())
    {
        setAssistantWorking(false);
        return;
    }

    assistantClearPatchPending = true;
    assistantButton->setEnabled(false);
    assistantClearPatchButton->setEnabled(false);
    assistantConnectionButton->setEnabled(false);
    assistantStatus->setText("Clearing the current patch and loading the default...",
                             juce::dontSendNotification);
}

bool SurgeSynthEditor::enqueueDefaultPatch()
{
    const auto *presetData = SurgeXTBinary::Init_Saw_fxp;
    constexpr auto headerSize = sizeof(sst::io::fxChunkSetCustom);
    constexpr auto presetSize = static_cast<std::size_t>(SurgeXTBinary::Init_Saw_fxpSize);
    if (presetSize <= headerSize)
    {
        assistantStatus->setText("Surge XT's embedded default patch is invalid.",
                                 juce::dontSendNotification);
        return false;
    }

    sst::io::fxChunkSetCustom presetHeader{};
    std::memcpy(&presetHeader, presetData, headerSize);

    auto chunkSize = sst::basic_blocks::mechanics::endian_read_int32BE(presetHeader.chunkSize);
    auto validPreset =
        chunkSize > 0 && static_cast<std::size_t>(chunkSize) <= presetSize - headerSize &&
        sst::basic_blocks::mechanics::endian_read_int32BE(presetHeader.chunkMagic) == 'CcnK' &&
        sst::basic_blocks::mechanics::endian_read_int32BE(presetHeader.fxMagic) == 'FPCh' &&
        sst::basic_blocks::mechanics::endian_read_int32BE(presetHeader.fxID) == 'cjs3';
    if (!validPreset)
    {
        assistantStatus->setText("Surge XT's embedded default patch is invalid.",
                                 juce::dontSendNotification);
        return false;
    }

    auto *synth = processor.surge.get();
    sge->undoManager()->pushPatch();
    synth->patch_loaded = false;
    assistantPatchLoadSequence = synth->enqueuePatchForLoad(presetData + headerSize, chunkSize);
    synth->processAudioThreadOpsWhenAudioEngineUnavailable();
    return true;
}

void SurgeSynthEditor::setAssistantWorking(bool working)
{
    auto cancelling = working && assistantCancellationRequested;
    auto canCancel =
        working && assistantPendingAction == AssistantPendingAction::Generate && !cancelling;
    assistantButton->setButtonText(cancelling  ? "Cancelling..."
                                   : canCancel ? "Cancel\nRequest"
                                               : (working ? "Working..." : "Ask\nAssistant"));
    assistantButton->setTitle(cancelling  ? "Cancelling assistant request"
                              : canCancel ? "Cancel assistant request"
                                          : (working ? "Assistant is working" : "Ask Assistant"));
    assistantButton->setEnabled(!working || canCancel);
    assistantClearPatchButton->setEnabled(!working);
    assistantConnectionButton->setEnabled(!working);
}

void SurgeSynthEditor::cancelAssistantRequest()
{
    if (assistantPendingAction != AssistantPendingAction::Generate)
        return;

    if (!assistantClient->cancel())
    {
        pollAssistantResult();
        return;
    }
    assistantCancellationRequested = true;
    setAssistantWorking(true);
    assistantStatus->setText("Cancelling assistant request...", juce::dontSendNotification);
}

void SurgeSynthEditor::showAssistantConnectionMenu()
{
    if (assistantConnectionOverlay || assistantPendingAction != AssistantPendingAction::None)
        return;

    struct ModelChoice
    {
        int id;
        Surge::Assistant::Provider provider;
        juce::String model;
    };
    struct ConnectionChoice
    {
        int id;
        Surge::Assistant::Provider provider;
    };

    juce::PopupMenu menu;
    menu.addSectionHeader("Assistant connection");
    menu.addItem(1, "Local fallback", true, assistantProvider == Surge::Assistant::Provider::None);

    std::vector<ModelChoice> choices;
    std::vector<ConnectionChoice> connectionChoices;
    auto nextChoiceId = 100;
    auto nextConnectionId = 10;
    for (auto provider : Surge::Assistant::availableProviders())
    {
        juce::PopupMenu providerMenu;
        auto models = assistantModels[provider];
        auto selectedModel = provider == assistantProvider ? assistantModel : juce::String{};
        if (selectedModel.isNotEmpty() &&
            std::find(models.begin(), models.end(), selectedModel) == models.end())
            models.insert(models.begin(), selectedModel);

        for (const auto &model : models)
        {
            auto id = nextChoiceId++;
            choices.push_back({id, provider, model});
            providerMenu.addItem(id, Surge::Assistant::modelDisplayName(provider, model), true,
                                 provider == assistantProvider && model == assistantModel);
        }
        providerMenu.addSeparator();
        auto configureId = nextConnectionId++;
        connectionChoices.push_back({configureId, provider});
        providerMenu.addItem(configureId, provider == Surge::Assistant::Provider::ChatGPT
                                              ? "Sign in with ChatGPT..."
                                              : "Add or replace API key...");
        menu.addSubMenu(Surge::Assistant::providerDisplayName(provider), providerMenu);
    }

    if (assistantProvider != Surge::Assistant::Provider::None)
    {
        menu.addSeparator();
        menu.addItem(2, "Disconnect " + Surge::Assistant::providerDisplayName(assistantProvider));
    }

    auto safeThis = juce::Component::SafePointer<SurgeSynthEditor>(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(assistantConnectionButton.get())
            .withParentComponent(this),
        [safeThis, choices, connectionChoices](int selected) {
            if (safeThis == nullptr || selected == 0)
                return;
            if (selected == 1)
            {
                safeThis->assistantProvider = Surge::Assistant::Provider::None;
                safeThis->assistantModel.clear();
                Surge::Storage::updateUserDefaultValue(&safeThis->processor.surge->storage,
                                                       Surge::Storage::AssistantProvider, "none");
                Surge::Storage::updateUserDefaultValue(&safeThis->processor.surge->storage,
                                                       Surge::Storage::AssistantModel, "");
                safeThis->updateAssistantConnectionButton();
                safeThis->assistantStatus->setText("Using the local deterministic fallback.",
                                                   juce::dontSendNotification);
                return;
            }
            if (selected == 2)
            {
                safeThis->disconnectAssistantConnection();
                return;
            }
            auto connection =
                std::find_if(connectionChoices.begin(), connectionChoices.end(),
                             [selected](const auto &choice) { return choice.id == selected; });
            if (connection != connectionChoices.end())
            {
                safeThis->showAssistantConnectionEditor(connection->provider);
                return;
            }

            auto found =
                std::find_if(choices.begin(), choices.end(),
                             [selected](const auto &choice) { return choice.id == selected; });
            if (found == choices.end())
                return;
            safeThis->assistantProvider = found->provider;
            safeThis->assistantModel = found->model;
            Surge::Storage::updateUserDefaultValue(
                &safeThis->processor.surge->storage, Surge::Storage::AssistantProvider,
                Surge::Assistant::providerId(found->provider).toStdString());
            Surge::Storage::updateUserDefaultValue(&safeThis->processor.surge->storage,
                                                   Surge::Storage::AssistantModel,
                                                   found->model.toStdString());
            safeThis->updateAssistantConnectionButton();
            safeThis->assistantStatus->setText(
                "Selected " + Surge::Assistant::providerDisplayName(found->provider) + " / " +
                    Surge::Assistant::modelDisplayName(found->provider, found->model) + ".",
                juce::dontSendNotification);
        });
}

void SurgeSynthEditor::showAssistantConnectionEditor(Surge::Assistant::Provider provider)
{
    if (assistantConnectionOverlay || provider == Surge::Assistant::Provider::None ||
        assistantPendingAction != AssistantPendingAction::None || assistantPatchPending ||
        assistantClearPatchPending || assistantClearPatchConfirmationPending)
        return;

    assistantConnectionOverlay = std::make_unique<AssistantConnectionOverlay>(provider);
    assistantConnectionOverlay->keyEditor().onFocusChanged = [this](bool hasFocus) {
        setAssistantPromptFocus(hasFocus);
    };
    assistantConnectionOverlay->onConnect = [this, provider](const juce::String &key) {
        beginAssistantConnection(provider, key);
    };

    auto safeThis = juce::Component::SafePointer<SurgeSynthEditor>(this);
    assistantConnectionOverlay->onCancel = [safeThis]() {
        if (safeThis == nullptr)
            return;
        auto overlay = juce::Component::SafePointer<AssistantConnectionOverlay>(
            safeThis->assistantConnectionOverlay.get());
        if (safeThis->assistantPendingAction == AssistantPendingAction::Connect)
        {
            if (!safeThis->assistantClient->cancel())
            {
                juce::MessageManager::callAsync([safeThis]() {
                    if (safeThis != nullptr)
                        safeThis->pollAssistantResult();
                });
                return;
            }
            safeThis->assistantCancellationRequested = true;
            safeThis->assistantConnectionOverlay->setCancelling();
            return;
        }
        safeThis->setAssistantPromptFocus(false);
        safeThis->assistantPrompt->setReadOnly(false);
        safeThis->setAssistantWorking(false);
        safeThis->assistantStatus->setText("Assistant connection cancelled.",
                                           juce::dontSendNotification);
        juce::MessageManager::callAsync([safeThis, overlay]() {
            if (safeThis != nullptr &&
                safeThis->assistantConnectionOverlay.get() == overlay.getComponent())
                safeThis->assistantConnectionOverlay.reset();
        });
    };

    addAndMakeVisible(*assistantConnectionOverlay);
    assistantConnectionOverlay->setBounds(getLocalBounds());
    assistantConnectionOverlay->toFront(false);
    assistantPrompt->setReadOnly(true);
    assistantButton->setEnabled(false);
    assistantClearPatchButton->setEnabled(false);
    assistantConnectionButton->setEnabled(false);
    assistantConnectionOverlay->focusInitialControl();
}

void SurgeSynthEditor::beginAssistantConnection(Surge::Assistant::Provider provider,
                                                const juce::String &apiKey)
{
    if (!assistantConnectionOverlay || assistantPendingAction != AssistantPendingAction::None)
        return;

    assistantConnectionOverlay->setBusy(true);
    assistantCancellationRequested = false;
    assistantPendingProvider = provider;
    assistantPendingAction = AssistantPendingAction::Connect;
    assistantFuture = assistantClient->connect(provider, apiKey);
}

void SurgeSynthEditor::disconnectAssistantConnection()
{
    if (assistantProvider == Surge::Assistant::Provider::None ||
        assistantPendingAction != AssistantPendingAction::None)
        return;

    assistantPendingProvider = assistantProvider;
    assistantPendingAction = AssistantPendingAction::Disconnect;
    assistantCancellationRequested = false;
    assistantButton->setEnabled(false);
    assistantClearPatchButton->setEnabled(false);
    assistantConnectionButton->setEnabled(false);
    assistantStatus->setText("Disconnecting " +
                                 Surge::Assistant::providerDisplayName(assistantProvider) + "...",
                             juce::dontSendNotification);
    assistantFuture = assistantClient->disconnect(assistantProvider);
}

void SurgeSynthEditor::pollAssistantResult()
{
    if (assistantPendingAction == AssistantPendingAction::None || !assistantFuture.valid() ||
        assistantFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    try
    {
        handleAssistantResult(assistantFuture.get());
    }
    catch (const std::exception &error)
    {
        Surge::Assistant::Result result;
        result.kind = assistantPendingAction == AssistantPendingAction::Generate
                          ? Surge::Assistant::ResultKind::Generation
                          : Surge::Assistant::ResultKind::Connection;
        result.error = "Assistant request failed: " + juce::String(error.what());
        handleAssistantResult(std::move(result));
    }
}

void SurgeSynthEditor::handleAssistantResult(Surge::Assistant::Result result)
{
    auto action = assistantPendingAction;
    auto cancellationRequested = assistantCancellationRequested;
    assistantCancellationRequested = false;
    assistantPendingAction = AssistantPendingAction::None;

    if (action == AssistantPendingAction::Connect)
    {
        if (cancellationRequested && !result.ok)
        {
            setAssistantPromptFocus(false);
            assistantConnectionOverlay.reset();
            assistantPrompt->setReadOnly(false);
            setAssistantWorking(false);
            assistantStatus->setText(result.error.isNotEmpty() ? result.error
                                                               : "Assistant connection cancelled.",
                                     juce::dontSendNotification);
            return;
        }
        if (!result.ok)
        {
            if (assistantConnectionOverlay)
            {
                assistantConnectionOverlay->setBusy(false);
                assistantConnectionOverlay->setStatus(result.error);
            }
            return;
        }

        assistantProvider = assistantPendingProvider;
        assistantModels[assistantProvider] = result.models;
        auto preferred = Surge::Assistant::providerDefaultModel(assistantProvider);
        auto foundPreferred = std::find(result.models.begin(), result.models.end(), preferred);
        assistantModel =
            foundPreferred != result.models.end() ? *foundPreferred : result.models.front();
        Surge::Storage::updateUserDefaultValue(
            &processor.surge->storage, Surge::Storage::AssistantProvider,
            Surge::Assistant::providerId(assistantProvider).toStdString());
        Surge::Storage::updateUserDefaultValue(&processor.surge->storage,
                                               Surge::Storage::AssistantModel,
                                               assistantModel.toStdString());
        setAssistantPromptFocus(false);
        assistantConnectionOverlay.reset();
        assistantPrompt->setReadOnly(false);
        setAssistantWorking(false);
        updateAssistantConnectionButton();
        auto connectionMessage =
            assistantProvider == Surge::Assistant::Provider::ChatGPT
                ? juce::String("Connected to ChatGPT Plus through Codex.")
                : "Connected to " + Surge::Assistant::providerDisplayName(assistantProvider) +
                      (result.credentialPersistent
                           ? ". The API key is in the OS credential store."
                           : ". The API key is available for this session only.");
        assistantStatus->setText(connectionMessage, juce::dontSendNotification);
        return;
    }

    if (action == AssistantPendingAction::Disconnect)
    {
        assistantButton->setEnabled(true);
        assistantClearPatchButton->setEnabled(true);
        assistantConnectionButton->setEnabled(true);
        if (!result.ok)
        {
            assistantStatus->setText(result.error, juce::dontSendNotification);
            return;
        }
        assistantProvider = Surge::Assistant::Provider::None;
        assistantModel.clear();
        Surge::Storage::updateUserDefaultValue(&processor.surge->storage,
                                               Surge::Storage::AssistantProvider, "none");
        Surge::Storage::updateUserDefaultValue(&processor.surge->storage,
                                               Surge::Storage::AssistantModel, "");
        updateAssistantConnectionButton();
        assistantStatus->setText("Disconnected. Using the local deterministic fallback.",
                                 juce::dontSendNotification);
        return;
    }

    if (action != AssistantPendingAction::Generate)
        return;

    assistantGenerationPending = false;
    setAssistantWorking(false);
    if (cancellationRequested)
    {
        assistantStatus->setText(result.error.isNotEmpty() ? result.error
                                                           : "Assistant request cancelled.",
                                 juce::dontSendNotification);
        return;
    }
    if (!result.ok)
    {
        assistantStatus->setText(result.error, juce::dontSendNotification);
        return;
    }
    if (!currentPatchMatchesAssistantSnapshot())
    {
        assistantStatus->setText("The patch changed while the model was responding, so its stale "
                                 "result was not applied.",
                                 juce::dontSendNotification);
        return;
    }
    if (applyAssistantPatchPlan(result.plan))
    {
        processor.assistantPromptHistory.push_back(assistantRequestPrompt);
        while (processor.assistantPromptHistory.size() > assistantConversationLimit)
            processor.assistantPromptHistory.erase(processor.assistantPromptHistory.begin());
    }
}

void SurgeSynthEditor::updateAssistantConnectionButton()
{
    if (!assistantConnectionButton)
        return;
    auto connection =
        assistantProvider == Surge::Assistant::Provider::None
            ? juce::String("Local fallback")
            : Surge::Assistant::providerDisplayName(assistantProvider) + " / " +
                  Surge::Assistant::modelDisplayName(assistantProvider, assistantModel);
    assistantConnectionButton->setButtonText(juce::String::charToString(0x2699));
    assistantConnectionButton->setTitle("Assistant settings. Current: " + connection);
    assistantConnectionButton->setDescription("Choose an assistant provider and model. Current: " +
                                              connection);
}

Surge::Assistant::PatchRequest
SurgeSynthEditor::buildAssistantPatchRequest(const juce::String &prompt, bool freshPatch)
{
    Surge::Assistant::PatchRequest request;
    request.provider = assistantProvider;
    request.model = assistantModel;
    request.prompt = prompt;
    request.freshPatch = freshPatch;
    auto firstPrompt =
        processor.assistantPromptHistory.size() >= assistantConversationLimit
            ? processor.assistantPromptHistory.size() - (assistantConversationLimit - 1)
            : 0;
    request.previousPrompts.assign(processor.assistantPromptHistory.begin() + firstPrompt,
                                   processor.assistantPromptHistory.end());

    auto *synth = processor.surge.get();
    auto &patch = synth->storage.getPatch();
    request.patchName = patch.name;

    assistantPatchSnapshot.parameterValues.clear();
    assistantPatchSnapshot.parameterValues.reserve(patch.param_ptr.size());
    for (const auto *parameter : patch.param_ptr)
        assistantPatchSnapshot.parameterValues.push_back(parameter->get_value_f01());
    assistantPatchSnapshot.name = patch.name;
    assistantPatchSnapshot.category = patch.category;
    assistantPatchSnapshot.patchId = synth->patchid;
    assistantPatchSnapshot.dirty = patch.isDirty;

    for (auto *parameter : patch.param_ptr)
    {
        if ((parameter->ctrlstyle & Surge::ParamConfig::kHide) != 0)
            continue;

        if (parameter->ctrlgroup == cg_FX && parameter->ctrlgroup_entry >= 0 &&
            parameter->ctrlgroup_entry < n_fx_slots)
        {
            auto &effect = patch.fx[parameter->ctrlgroup_entry];
            if (effect.type.val.i == fxt_off && parameter != &effect.type)
                continue;
        }

        auto synthId = synth->idForParameter(parameter);
        auto id = synthId.getSynthSideId();
        std::array<char, 256> name{};
        synth->getParameterNameExtendedByFXGroup(synthId, name.data());

        Surge::Assistant::ParameterInfo info;
        info.id = id;
        info.name = juce::String::fromUTF8(name.data());
        info.display = parameter->get_display();
        info.currentValue = parameter->get_value_f01();
        info.defaultValue = parameter->get_default_value_f01();

        if (parameter->valtype != vt_float)
        {
            auto minimum = static_cast<std::int64_t>(parameter->val_min.i);
            auto maximum = static_cast<std::int64_t>(parameter->val_max.i);
            auto optionCount = maximum - minimum + 1;
            if (optionCount > 0 && optionCount <= 65)
            {
                info.options.reserve(static_cast<std::size_t>(optionCount));
                for (auto value = minimum; value <= maximum; ++value)
                {
                    auto normalized = parameter->value_to_normalized(static_cast<float>(value));
                    info.options.push_back({normalized, parameter->get_display(true, normalized)});
                }
            }
        }
        request.parameters.push_back(std::move(info));
    }
    return request;
}

bool SurgeSynthEditor::currentPatchMatchesAssistantSnapshot() const
{
    auto *synth = processor.surge.get();
    const auto &patch = synth->storage.getPatch();
    if (synth->patchid != assistantPatchSnapshot.patchId ||
        patch.name != assistantPatchSnapshot.name ||
        patch.category != assistantPatchSnapshot.category ||
        patch.isDirty != assistantPatchSnapshot.dirty ||
        patch.param_ptr.size() != assistantPatchSnapshot.parameterValues.size())
        return false;

    for (std::size_t index = 0; index < patch.param_ptr.size(); ++index)
        if (std::abs(patch.param_ptr[index]->get_value_f01() -
                     assistantPatchSnapshot.parameterValues[index]) > 1.0e-6f)
            return false;
    return true;
}

bool SurgeSynthEditor::applyAssistantPatchPlan(const Surge::Assistant::PatchPlan &plan)
{
    auto *synth = processor.surge.get();
    auto &patch = synth->storage.getPatch();
    std::vector<Surge::Assistant::PatchOperation> changes;
    for (const auto &operation : plan.operations)
    {
        if (operation.parameterId < 0 || operation.parameterId >= patch.param_ptr.size() ||
            !std::isfinite(operation.value) || operation.value < 0.0f || operation.value > 1.0f)
        {
            assistantStatus->setText("The model returned an invalid patch operation.",
                                     juce::dontSendNotification);
            return false;
        }
        auto candidate = *patch.param_ptr[operation.parameterId];
        candidate.set_value_f01(operation.value);
        auto canonicalValue = candidate.get_value_f01();
        if (std::abs(patch.param_ptr[operation.parameterId]->get_value_f01() - canonicalValue) >
            1.0e-6f)
            changes.push_back({operation.parameterId, canonicalValue});
    }

    if (changes.empty())
    {
        assistantStatus->setText("The model's plan did not change any current settings.",
                                 juce::dontSendNotification);
        return false;
    }

    // A response may arrive while the user has a menu open. Close it before rebuilding
    // controls so JUCE cannot leave its window pointing at destroyed menu components.
    juce::PopupMenu::dismissAllActiveMenus();
    sge->undoManager()->pushPatch();
    for (const auto &operation : changes)
        synth->setParameter01(synth->idForParameter(patch.param_ptr[operation.parameterId]),
                              operation.value, true);
    synth->processAudioThreadOpsWhenAudioEngineUnavailable();

    if (assistantRequestWasFresh)
    {
        patch.name =
            (plan.name.isNotEmpty() ? plan.name : juce::String("Assistant Patch")).toStdString();
        patch.category = "Assistant";
        patch.author = "Surge XT Assistant";
        patch.comment =
            ("Generated with " + Surge::Assistant::providerDisplayName(assistantProvider) + " / " +
             assistantModel + " from: " + assistantRequestPrompt)
                .substring(0, 512)
                .toStdString();
        patch.tags.clear();
    }
    patch.isDirty = true;
    synth->patchChanged = true;
    sge->queueRebuildUI();
    assistantStatus->toFront(false);
    assistantStatus->setText(plan.summary, juce::dontSendNotification);
    return true;
}

bool SurgeSynthEditor::isCurrentPatchUntouchedInit() const
{
    const auto *synth = processor.surge.get();
    const auto &storage = synth->storage;
    const auto &patch = storage.getPatch();

    if (patch.isDirty)
        return false;

    if (synth->patchid < 0)
        return (patch.name == "Init" && patch.category == "Init") ||
               (patch.name == storage.initPatchName && patch.category == storage.initPatchCategory);

    if (patch.name != storage.initPatchName || patch.category != storage.initPatchCategory)
        return false;

    if (synth->patchid >= storage.patch_list.size())
        return false;

    const auto &listedPatch = storage.patch_list[synth->patchid];
    const auto &listedCategory = storage.patch_category[listedPatch.category];
    const auto expectsFactoryPatch = storage.initPatchCategoryType == "Factory";
    return listedPatch.name == storage.initPatchName &&
           listedCategory.name == storage.initPatchCategory &&
           listedCategory.isFactory == expectsFactoryPatch;
}

void SurgeSynthEditor::applyCasioRetroKeyboardPatch()
{
    auto *synth = processor.surge.get();
    auto &patch = synth->storage.getPatch();
    auto &scene = patch.scene[0];

    auto setValue = [synth](Parameter &parameter, float value) {
        synth->setParameter01(synth->idForParameter(&parameter),
                              parameter.value_to_normalized(value), true);
    };

    setValue(patch.scene_active, 0);
    setValue(patch.scenemode, sm_single);
    setValue(patch.character, cm_bright);
    setValue(patch.volume, -6.0f);
    setValue(patch.fx_bypass, fxb_all_fx);

    for (auto &effect : patch.fx)
    {
        setValue(effect.type, fxt_off);
    }

    setValue(scene.octave, 0);
    setValue(scene.pitch, 0.0f);
    setValue(scene.polymode, pm_poly);
    setValue(scene.portamento, scene.portamento.val_min.f);
    setValue(scene.fm_switch, fm_off);
    setValue(scene.drift, 0.025f);
    setValue(scene.volume, 0.86f);
    setValue(scene.pan, 0.0f);
    setValue(scene.width, 0.55f);
    setValue(scene.vca_level, -3.0f);
    setValue(scene.vca_velsense, -15.0f);

    auto &osc1 = scene.osc[0];
    setValue(osc1.type, ot_classic);
    setValue(osc1.octave, 0);
    setValue(osc1.pitch, 0.0f);
    setValue(osc1.keytrack, 1);
    setValue(osc1.retrigger, 1);
    setValue(osc1.p[ClassicOscillator::co_shape], 1.0f);
    setValue(osc1.p[ClassicOscillator::co_width1], 0.5f);
    setValue(osc1.p[ClassicOscillator::co_width2], 0.5f);
    setValue(osc1.p[ClassicOscillator::co_mainsubmix], 0.18f);
    setValue(osc1.p[ClassicOscillator::co_sync], 0.0f);
    setValue(osc1.p[ClassicOscillator::co_unison_detune], 0.1f);
    setValue(osc1.p[ClassicOscillator::co_unison_voices], 1);

    auto &osc2 = scene.osc[1];
    setValue(osc2.type, ot_classic);
    setValue(osc2.octave, 1);
    setValue(osc2.pitch, 0.04f);
    setValue(osc2.keytrack, 1);
    setValue(osc2.retrigger, 1);
    setValue(osc2.p[ClassicOscillator::co_shape], 0.72f);
    setValue(osc2.p[ClassicOscillator::co_width1], 0.34f);
    setValue(osc2.p[ClassicOscillator::co_width2], 0.34f);
    setValue(osc2.p[ClassicOscillator::co_mainsubmix], 0.0f);
    setValue(osc2.p[ClassicOscillator::co_sync], 0.0f);
    setValue(osc2.p[ClassicOscillator::co_unison_detune], 0.035f);
    setValue(osc2.p[ClassicOscillator::co_unison_voices], 2);

    setValue(scene.level_o1, 0.78f);
    setValue(scene.level_o2, 0.24f);
    setValue(scene.mute_o1, 0);
    setValue(scene.mute_o2, 0);
    setValue(scene.mute_o3, 1);
    setValue(scene.mute_noise, 1);
    setValue(scene.mute_ring_12, 1);
    setValue(scene.mute_ring_23, 1);
    setValue(scene.route_o1, 1);
    setValue(scene.route_o2, 1);

    setValue(scene.filterblock_configuration, fc_serial1);
    setValue(scene.filterunit[0].type, sst::filters::fut_lp12);
    setValue(scene.filterunit[0].subtype, 0);
    setValue(scene.filterunit[0].cutoff, 38.0f);
    setValue(scene.filterunit[0].resonance, 0.12f);
    setValue(scene.filterunit[0].envmod, 19.0f);
    setValue(scene.filterunit[0].keytrack, 0.35f);
    setValue(scene.filterunit[1].type, sst::filters::fut_none);
    setValue(scene.feedback, 0.0f);
    setValue(scene.filter_balance, 0.0f);
    setValue(scene.lowcut, scene.lowcut.val_min.f);

    auto setEnvelope = [&setValue](ADSRStorage &envelope, float attack, float decay, float sustain,
                                   float release) {
        setValue(envelope.a, attack);
        setValue(envelope.d, decay);
        setValue(envelope.s, sustain);
        setValue(envelope.r, release);
        setValue(envelope.a_s, 1);
        setValue(envelope.d_s, 1);
        setValue(envelope.r_s, 1);
        setValue(envelope.mode, emt_digital);
    };

    setEnvelope(scene.adsr[adsr_ampeg], -8.0f, -0.8f, 0.42f, -2.0f);
    setEnvelope(scene.adsr[adsr_filteg], -8.0f, -1.4f, 0.08f, -2.5f);

    patch.name = "Casio Retro Keyboard";
    patch.category = "Assistant";
    patch.author = "Surge XT Assistant";
    patch.comment = "Generated locally from the prompt: casio retro keyboard sound";
    patch.tags.clear();
    patch.isDirty = true;
    synth->patchChanged = true;
    sge->queueRebuildUI();

    assistantStatus->toFront(false);
    assistantStatus->setText(
        "Created Casio Retro Keyboard: pulse layers, plucky envelopes, and a low-pass filter.",
        juce::dontSendNotification);
}

void SurgeSynthEditor::applyMoreReverb()
{
    auto *synth = processor.surge.get();
    auto &patch = synth->storage.getPatch();

    auto finishChange = [this, synth, &patch](const juce::String &status) {
        patch.isDirty = true;
        synth->patchChanged = true;
        sge->queueRebuildUI();
        assistantStatus->toFront(false);
        assistantStatus->setText(status, juce::dontSendNotification);
    };

    for (auto &effect : patch.fx)
    {
        auto type = effect.type.val.i;
        if (type != fxt_reverb && type != fxt_reverb2 && type != fxt_spring_reverb)
            continue;

        for (auto &parameter : effect.p)
        {
            if (std::strcmp(parameter.get_name(), "Mix") != 0)
                continue;

            auto currentMix = parameter.get_value_f01();
            auto increasedMix = std::min(1.0f, currentMix + 0.1f);
            if (increasedMix == currentMix)
            {
                assistantStatus->setText("Reverb mix is already at maximum; no settings changed.",
                                         juce::dontSendNotification);
                return;
            }

            sge->undoManager()->pushPatch();
            synth->setParameter01(synth->idForParameter(&parameter), increasedMix, true);
            finishChange(
                "Increased the current reverb mix slightly; all other settings are unchanged.");
            return;
        }
    }

    constexpr int globalFxSlots[]{fxslot_global1, fxslot_global2, fxslot_global3, fxslot_global4};
    for (auto slot : globalFxSlots)
    {
        auto &effect = patch.fx[slot];
        if (effect.type.val.i != fxt_off)
            continue;

        sge->undoManager()->pushPatch();
        synth->setParameter01(synth->idForParameter(&effect.type),
                              effect.type.value_to_normalized(fxt_reverb2), true);
        synth->processAudioThreadOpsWhenAudioEngineUnavailable();
        finishChange(juce::String("Added Reverb 2 in ") + fxslot_shortnames[slot] +
                     "; all other settings are unchanged.");
        return;
    }

    assistantStatus->setText("No empty global FX slot is available; no settings changed.",
                             juce::dontSendNotification);
}

void SurgeSynthEditor::setAssistantPromptFocus(bool hasFocus)
{
    if (!sge || hasFocus == assistantPromptHasFocus)
        return;

    assistantPromptHasFocus = hasFocus;
    if (hasFocus)
    {
        if (keyboard)
            keyboard->focusLost(juce::Component::FocusChangeType::focusChangedDirectly);
        ++sge->vkbForward;
    }
    else
    {
        jassert(sge->vkbForward > 0);
        if (sge->vkbForward > 0)
            --sge->vkbForward;
    }
}

void SurgeSynthEditor::parentHierarchyChanged()
{
    reapplySurgeComponentColours();

#if WINDOWS
    auto swr = Surge::Storage::getUserDefaultValue(&(this->processor.surge->storage),
                                                   Surge::Storage::UseSoftwareRenderer, false);

    if (swr)
    {
        if (auto peer = getPeer())
        {
            // 0 for software mode, 1 for Direct2D mode
            peer->setCurrentRenderingEngine(0);
        }
    }
#endif
}

void SurgeSynthEditor::IdleTimer::timerCallback() { ed->idle(); }

void SurgeSynthEditor::populateForStreaming(SurgeSynthesizer *s)
{
    if (sge)
        sge->populateDawExtraState(s);
}

void SurgeSynthEditor::populateFromStreaming(SurgeSynthesizer *s)
{
    if (sge)
        sge->loadFromDawExtraState(s);
}

bool SurgeSynthEditor::isInterestedInFileDrag(const juce::StringArray &files)
{
    if (files.size() != 1)
        return false;

    auto *fxw =
        dynamic_cast<Surge::Widgets::CurrentFxDisplay *>(sge->frame->getControlGroupLayer(cg_FX));

    for (auto i = files.begin(); i != files.end(); ++i)
    {
        if (fxw->canDropTarget(*i))
            return true;

        if (sge->canDropTarget(i->toStdString()))
            return true;
    }
    return false;
}

void SurgeSynthEditor::filesDropped(const juce::StringArray &files, int x, int y)
{
    if (files.size() != 1)
        return;

    auto *fxw =
        dynamic_cast<Surge::Widgets::CurrentFxDisplay *>(sge->frame->getControlGroupLayer(cg_FX));

    for (auto i = files.begin(); i != files.end(); ++i)
    {
        if (fxw->canDropTarget(*i) &&
            fxw->reallyContains(fxw->getLocalPoint(this, juce::Point<int>(x, y)), true))
            fxw->onDrop(*i);
        else if (sge->canDropTarget(i->toStdString()))
            sge->onDrop(i->toStdString());
    }
}

void SurgeSynthEditor::beginParameterEdit(Parameter *p)
{
    // std::cout << "BEGIN EDIT " << p->get_name() << std::endl;
    auto par = processor.paramsByID[processor.surge->idForParameter(p)];
    par->inEditGesture = true;
    par->beginChangeGesture();
}

void SurgeSynthEditor::endParameterEdit(Parameter *p)
{
    auto par = processor.paramsByID[processor.surge->idForParameter(p)];
    par->inEditGesture = false;
    par->endChangeGesture();
    if (fireListenersOnEndEdit)
        processor.paramChangeToListeners(p);
}

void SurgeSynthEditor::beginMacroEdit(long macroNum)
{
    auto par = processor.macrosById[macroNum];
    par->beginChangeGesture();
}

void SurgeSynthEditor::endMacroEdit(long macroNum)
{
    auto par = processor.macrosById[macroNum];
    par->endChangeGesture();
    // echo change to OSC out
    float newval = par->getValue();
    processor.paramChangeToListeners(nullptr, true, processor.SCT_MACRO, (float)macroNum, newval,
                                     .0, "");
}

#if LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

juce::PopupMenu SurgeSynthEditor::modifyHostMenu(juce::PopupMenu menu)
{
    // make things look a bit nicer for our friends from Image-Line
    if (juce::PluginHostType().isFruityLoops())
    {
        auto it = juce::PopupMenu::MenuItemIterator(menu);

        while (it.next())
        {
            auto txt = it.getItem().text;

            if (txt.startsWithChar('-'))
            {
                it.getItem().isSectionHeader = true;
                it.getItem().text = txt.fromFirstOccurrenceOf("-", false, false);
            }
        }

        return menu;
    }

    // we really don't need that parameter name repeated in Reaper...
    if (juce::PluginHostType().isReaper())
    {
        auto newMenu = juce::PopupMenu();
        auto it = juce::PopupMenu::MenuItemIterator(menu);

        while (it.next())
        {
            auto txt = it.getItem().text;
            bool include = true;

            if (txt.startsWithChar('[') && txt.endsWithChar(']'))
            {
                include = it.next();
            }

            if (include)
            {
                newMenu.addItem(it.getItem());
            }
        }

        return newMenu;
    }

    return menu;
}

juce::PopupMenu SurgeSynthEditor::hostMenuFor(Parameter *p)
{
    if (sge)
    {
        if (sge->synth->hostProgram.compare("Unknown") == 0)
        {
            return juce::PopupMenu();
        }
    }

    auto par = processor.paramsByID[processor.surge->idForParameter(p)];

    if (auto *c = getHostContext())
    {
        if (auto menuInfo = c->getContextMenuForParameterIndex(par))
        {
            auto menu = menuInfo->getEquivalentPopupMenu();

            menu = modifyHostMenu(menu);

            return menu;
        }
    }

    return juce::PopupMenu();
}

juce::PopupMenu SurgeSynthEditor::hostMenuForMacro(int macro)
{
    if (sge)
    {
        if (sge->synth->hostProgram.compare("Unknown") == 0)
        {
            return juce::PopupMenu();
        }
    }

    auto par = processor.macrosById[macro];

    if (auto *c = getHostContext())
    {
        if (auto menuInfo = c->getContextMenuForParameterIndex(par))
        {
            auto menu = menuInfo->getEquivalentPopupMenu();

            modifyHostMenu(menu);

            return menu;
        }
    }

    return juce::PopupMenu();
}

#if LINUX
#pragma GCC diagnostic pop
#endif

bool SurgeSynthEditor::keyPressed(const juce::KeyPress &key, juce::Component *orig)
{
    /*
     * So, sigh. On Linux/Windows the keyStateChanged event of the parent isn't suppressed
     * when the keyStateChange is false. But the MIDI keyboard rescans on state change.
     * So what we need to do is: forward all keystate trues (which will come before a
     * keypress but only if the keypress is not in a text edit) but only forward keystate
     * false if I have sent at least one key through this fallthrough mechanism. So:
     */
    if (sge->getShowVirtualKeyboard())
    {
        bool shortcutsUsed = sge->getUseKeyboardShortcuts();
        auto mapMatch = sge->keyMapManager->matches(key);

        if (mapMatch.has_value())
        {
            if (shortcutsUsed)
            {
                auto action = *mapMatch;

                switch (action)
                {
                case Surge::GUI::VKB_OCTAVE_DOWN:
                    midiKeyboardOctave = std::clamp(midiKeyboardOctave - 1, 0, 9);
                    keyboard->setKeyPressBaseOctave(midiKeyboardOctave);
                    return true;
                case Surge::GUI::VKB_OCTAVE_UP:
                    midiKeyboardOctave = std::clamp(midiKeyboardOctave + 1, 0, 9);
                    keyboard->setKeyPressBaseOctave(midiKeyboardOctave);
                    return true;
                case Surge::GUI::VKB_VELOCITY_DOWN_10PCT:
                    midiKeyboardVelocity = std::clamp(midiKeyboardVelocity - 0.1f, 0.f, 1.f);
                    keyboard->setVelocity(midiKeyboardVelocity, true);
                    return true;
                case Surge::GUI::VKB_VELOCITY_UP_10PCT:
                    midiKeyboardVelocity = std::clamp(midiKeyboardVelocity + 0.1f, 0.f, 1.f);
                    keyboard->setVelocity(midiKeyboardVelocity, true);
                    return true;
                default:
                    break;
                }
            }
        }

        if (sge->shouldForwardKeysToVKB() && orig != keyboard.get())
        {
            return keyboard->keyPressed(key);
        }
    }

    return false;
}

bool SurgeSynthEditor::keyStateChanged(bool isKeyDown, juce::Component *originatingComponent)
{
    if (sge->getShowVirtualKeyboard() && sge->shouldForwardKeysToVKB() &&
        originatingComponent != keyboard.get())
    {
        return keyboard->keyStateChanged(isKeyDown);
    }

    return false;
}

void SurgeSynthEditor::setPitchModSustainGUI(int pitch, int mod, int sus)
{
    auto pw = dynamic_cast<VKeyboardWheel *>(pitchwheel.get());
    if (pw)
    {
        pw->value = pitch;
        pw->repaint();
    }
    auto mw = dynamic_cast<VKeyboardWheel *>(modwheel.get());
    if (mw)
    {
        mw->value = mod;
        mw->repaint();
    }
    auto sw = dynamic_cast<VKeyboardSus *>(suspedal.get());
    if (sw)
    {
        sw->isOn = sus > 64;
        sw->repaint();
    }
}
