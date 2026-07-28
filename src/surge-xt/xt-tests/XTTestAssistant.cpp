/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later).
 */

#include "catch2/catch_amalgamated.hpp"
#include "SurgeSynthEditor.h"
#include <cstring>
#include <vector>

TEST_CASE("Assistant creates the Casio retro keyboard patch", "[xt-assistant]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    auto processor = std::make_unique<SurgeSynthProcessor>();
    auto editor = std::make_unique<SurgeSynthEditor>(*processor);
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
