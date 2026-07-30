/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later).
 */

#include "catch2/catch_amalgamated.hpp"
#include "AssistantClient.h"
#include "SurgeSynthEditor.h"
#include "gui/UndoManager.h"
#include <chrono>
#include <cstring>
#include <set>
#include <vector>

TEST_CASE("Assistant validates model patch plans", "[xt-assistant]")
{
    const std::set<int> allowedIds{4, 8};

    auto valid = Surge::Assistant::parsePatchPlanJson(
        R"json(```json
        {"name":"Warm Keys","summary":"Softened the filter.","operations":[
          {"parameter_id":4,"value":0.25},{"parameter_id":8,"value":1.0}
        ]}
        ```)json",
        true, allowedIds);
    REQUIRE(valid.ok);
    CHECK(valid.plan.name == "Warm Keys");
    CHECK(valid.plan.summary == "Softened the filter.");
    REQUIRE(valid.plan.operations.size() == 2);
    CHECK(valid.plan.operations[0].parameterId == 4);
    CHECK(valid.plan.operations[0].value == Catch::Approx(0.25f));

    auto unknown = Surge::Assistant::parsePatchPlanJson(
        R"json({"operations":[{"parameter_id":99,"value":0.5}]})json", true, allowedIds);
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error.containsIgnoreCase("unknown parameter"));

    auto outOfRange = Surge::Assistant::parsePatchPlanJson(
        R"json({"operations":[{"parameter_id":4,"value":1.5}]})json", true, allowedIds);
    CHECK_FALSE(outOfRange.ok);
    CHECK(outOfRange.error.containsIgnoreCase("valid range"));

    auto duplicate = Surge::Assistant::parsePatchPlanJson(
        R"json({"operations":[{"parameter_id":4,"value":0.2},{"parameter_id":4,"value":0.3}]})json",
        true, allowedIds);
    CHECK_FALSE(duplicate.ok);
    CHECK(duplicate.error.containsIgnoreCase("more than once"));

    auto followUp = Surge::Assistant::parseChatCompletion(
        R"json({"choices":[{"message":{"content":"{\"name\":\"Do not rename\",\"summary\":\"Small change\",\"operations\":[{\"parameter_id\":4,\"value\":0.3}]}"}}]})json",
        false, allowedIds);
    REQUIRE(followUp.ok);
    CHECK(followUp.plan.name.isEmpty());
}

TEST_CASE("Assistant creates, clears, and restores a patch", "[xt-assistant]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    auto processor = std::make_unique<SurgeSynthProcessor>();
    auto editor = std::make_unique<SurgeSynthEditor>(*processor);
    editor->assistantProvider = Surge::Assistant::Provider::None;
    editor->assistantModel.clear();
    REQUIRE(editor->isCurrentPatchUntouchedInit());
    editor->assistantPrompt->setText("casio retro keyboard sound", false);
    editor->submitAssistantPrompt();

    REQUIRE(editor->assistantPatchPending);
    editor->idle();
    REQUIRE_FALSE(editor->assistantPatchPending);

    auto &patch = processor->surge->storage.getPatch();
    auto &scene = patch.scene[0];
    CHECK(patch.name == "Casio Retro Keyboard");
    CHECK(patch.category == "Assistant");
    CHECK(patch.isDirty);
    CHECK(scene.osc[0].type.val.i == ot_classic);
    CHECK(scene.osc[1].type.val.i == ot_classic);
    CHECK_FALSE(scene.mute_o1.val.b);
    CHECK_FALSE(scene.mute_o2.val.b);
    CHECK(scene.mute_o3.val.b);
    CHECK(scene.filterunit[0].type.val.i == sst::filters::fut_lp12);
    CHECK_FALSE(editor->isCurrentPatchUntouchedInit());

    std::vector<float> parametersBeforeFollowUp;
    parametersBeforeFollowUp.reserve(patch.param_ptr.size());
    for (const auto *parameter : patch.param_ptr)
        parametersBeforeFollowUp.push_back(parameter->get_value_f01());

    editor->assistantPrompt->setText("a bit more reverb", false);
    editor->submitAssistantPrompt();

    CHECK_FALSE(editor->assistantPatchPending);
    CHECK(patch.name == "Casio Retro Keyboard");
    CHECK(patch.fx[fxslot_global1].type.val.i == fxt_reverb2);
    Parameter *reverbMix = nullptr;
    for (auto &parameter : patch.fx[fxslot_global1].p)
        if (std::strcmp(parameter.get_name(), "Mix") == 0)
            reverbMix = &parameter;
    REQUIRE(reverbMix != nullptr);
    CHECK(reverbMix->get_value_f01() == Catch::Approx(0.33f));

    for (std::size_t i = 0; i < patch.param_ptr.size(); ++i)
    {
        const auto *parameter = patch.param_ptr[i];
        auto isAddedReverbSetting =
            parameter->ctrlgroup == cg_FX && parameter->ctrlgroup_entry == fxslot_global1;
        if (!isAddedReverbSetting)
            CHECK(parameter->get_value_f01() == Catch::Approx(parametersBeforeFollowUp[i]));
    }

    processor->prepareToPlay(44100.0, 512);
    juce::AudioBuffer<float> audio(6, 512);
    juce::MidiBuffer midi;
    processor->processBlock(audio, midi);

    processor->assistantPromptHistory = {"make a keyboard", "add reverb"};
    editor->assistantPrompt->setText("warm pad for later", false);
    editor->assistantProvider = Surge::Assistant::Provider::ChatGPT;
    editor->assistantModel = "default";
    editor->beginClearPatch();
    REQUIRE(editor->assistantClearPatchPending);
    REQUIRE(processor->assistantPromptHistory.size() == 2);
    CHECK(processor->assistantPromptHistory[0] == "make a keyboard");
    CHECK(processor->assistantPromptHistory[1] == "add reverb");
    CHECK(editor->assistantPendingAction == SurgeSynthEditor::AssistantPendingAction::None);
    CHECK_FALSE(editor->assistantGenerationPending);
    CHECK_FALSE(editor->assistantPrompt->isEnabled());
    CHECK(editor->assistantButton->getButtonText() == "Ask\nAssistant");
    CHECK_FALSE(editor->assistantClearPatchButton->isEnabled());

    editor->idle();
    CHECK(editor->assistantClearPatchPending);
    audio.clear();
    processor->processBlock(audio, midi);
    editor->idle();

    CHECK_FALSE(editor->assistantClearPatchPending);
    CHECK_FALSE(editor->assistantGenerationPending);
    CHECK(editor->assistantPendingAction == SurgeSynthEditor::AssistantPendingAction::None);
    CHECK(editor->assistantPrompt->isEnabled());
    CHECK(editor->assistantPrompt->getText() == "warm pad for later");
    CHECK(editor->assistantStatus->getText().containsIgnoreCase("reset to default"));
    CHECK(editor->assistantButton->getButtonText() == "Ask\nAssistant");
    CHECK(editor->assistantClearPatchButton->isEnabled());
    CHECK(editor->assistantClearPatchButton->getButtonText() == "Clear\nPatch");
    CAPTURE(patch.name, patch.category, patch.isDirty, processor->surge->patchid,
            processor->surge->storage.initPatchName, processor->surge->storage.initPatchCategory);
    CHECK(editor->isCurrentPatchUntouchedInit());
    CHECK(patch.fx[fxslot_global1].type.val.i == fxt_off);
    REQUIRE(processor->assistantPromptHistory.size() == 2);

    REQUIRE(processor->undoManager->undo());
    audio.clear();
    processor->processBlock(audio, midi);
    editor->idle();
    CHECK(patch.fx[fxslot_global1].type.val.i == fxt_reverb2);

    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);

    double outputEnergy = 0.0;
    for (int block = 0; block < 16; ++block)
    {
        audio.clear();
        processor->processBlock(audio, midi);
        midi.clear();

        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                outputEnergy += std::abs(audio.getSample(channel, sample));
    }
    CHECK(outputEnergy > 0.01);

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    editor.reset();
    processor.reset();
}

TEST_CASE("Assistant builds its parameter catalog promptly", "[xt-assistant]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    auto processor = std::make_unique<SurgeSynthProcessor>();
    auto editor = std::make_unique<SurgeSynthEditor>(*processor);
    CHECK(editor->assistantPrompt->getWidth() == editor->getWidth() - 222);
    CHECK(editor->assistantClearPatchButton->getWidth() == 74);
    CHECK(editor->assistantButton->getWidth() == 74);
    CHECK(editor->assistantButton->getRight() + 2 == editor->assistantClearPatchButton->getX());
    for (int index = 0; index < 12; ++index)
        processor->assistantPromptHistory.push_back("Earlier request " + juce::String(index));
    auto start = std::chrono::steady_clock::now();
    auto request = editor->buildAssistantPatchRequest("warm analog brass", true);
    auto elapsed = std::chrono::steady_clock::now() - start;

    CAPTURE(request.parameters.size());
    CAPTURE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    CHECK_FALSE(request.parameters.empty());
    REQUIRE(request.previousPrompts.size() == 9);
    CHECK(request.previousPrompts.front() == "Earlier request 3");
    CHECK(request.previousPrompts.back() == "Earlier request 11");
    CHECK(elapsed < std::chrono::seconds(2));

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    editor.reset();
    processor.reset();
}
