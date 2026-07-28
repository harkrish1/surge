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

    assistantButton = std::make_unique<juce::TextButton>("Ask assistant");
    addAndMakeVisible(*assistantButton);
    assistantButton->setColour(juce::TextButton::buttonColourId, juce::Colour(238, 155, 30));
    assistantButton->setColour(juce::TextButton::buttonOnColourId, juce::Colour(255, 185, 60));
    assistantButton->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    assistantButton->setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    assistantButton->setMouseClickGrabsKeyboardFocus(false);
    assistantButton->onClick = [this]() { submitAssistantPrompt(); };

    auto promptEditor = std::make_unique<AssistantPromptEditor>("Assistant prompt");
    promptEditor->onFocusChanged = [this](bool hasFocus) { setAssistantPromptFocus(hasFocus); };
    assistantPrompt = std::move(promptEditor);
    assistantPrompt->setMultiLine(false);
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

    assistantStatus = std::make_unique<juce::Label>("Assistant status", "Local patch generator");
    assistantStatus->setJustificationType(juce::Justification::topLeft);
    assistantStatus->setColour(juce::Label::textColourId, juce::Colours::white);
    assistantStatus->setColour(juce::Label::backgroundColourId, juce::Colour(24, 24, 24));
    assistantStatus->setOpaque(true);
    assistantStatus->setFont(juce::Font(12.0f));
    assistantStatus->setText("Try \"casio retro keyboard sound\", then \"a bit more reverb\".",
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
    assistantButton->toFront(false);

    idleTimer = std::make_unique<IdleTimer>(this);
    idleTimer->startTimer(1000 / 60);
}

SurgeSynthEditor::~SurgeSynthEditor()
{
    idleTimer->stopTimer();

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
    if (assistantPatchPending)
    {
        if (!processor.surge->rawLoadEnqueued.load() && processor.surge->patch_loaded)
        {
            assistantPatchPending = false;
            assistantButton->setEnabled(true);
            applyCasioRetroKeyboardPatch();
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
    assistantButton->setBounds(assistantBar.removeFromRight(176));
    assistantBar.removeFromRight(8);
    assistantPrompt->setBounds(assistantBar);
    assistantPrompt->setIndents(
        8, std::max(0, (assistantPrompt->getHeight() - assistantPrompt->getTextHeight()) / 2));
    assistantStatus->setBounds(getLocalBounds()
                                   .withTop(assistantBarHeight)
                                   .withHeight(assistantResponseHeight)
                                   .reduced(8, 2));
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
    if (assistantPatchPending)
    {
        assistantStatus->setText("The assistant is already creating a patch.",
                                 juce::dontSendNotification);
        return;
    }

    auto prompt = assistantPrompt->getText().trim();
    if (prompt.isEmpty())
    {
        assistantStatus->setText("Enter a sound description first.", juce::dontSendNotification);
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

    auto *synth = processor.surge.get();
    const auto *presetData = SurgeXTBinary::Init_Saw_fxp;
    constexpr auto headerSize = sizeof(sst::io::fxChunkSetCustom);
    constexpr auto presetSize = static_cast<std::size_t>(SurgeXTBinary::Init_Saw_fxpSize);
    if (presetSize <= headerSize)
    {
        assistantStatus->setText("The assistant's embedded patch template is invalid.",
                                 juce::dontSendNotification);
        return;
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
        assistantStatus->setText("The assistant's embedded patch template is invalid.",
                                 juce::dontSendNotification);
        return;
    }

    sge->undoManager()->pushPatch();
    synth->patch_loaded = false;
    assistantPatchPending = true;
    assistantButton->setEnabled(false);
    assistantStatus->setText("Creating a Casio-style retro keyboard patch...",
                             juce::dontSendNotification);
    synth->enqueuePatchForLoad(presetData + headerSize, chunkSize);
    synth->processAudioThreadOpsWhenAudioEngineUnavailable();
}

bool SurgeSynthEditor::isCurrentPatchUntouchedInit() const
{
    const auto *synth = processor.surge.get();
    const auto &storage = synth->storage;
    const auto &patch = storage.getPatch();

    if (patch.isDirty)
        return false;

    if (synth->patchid < 0)
        return patch.name == "Init" && patch.category == "Init";

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
