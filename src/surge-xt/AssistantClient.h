/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later).
 */

#ifndef SURGE_SRC_SURGE_XT_ASSISTANTCLIENT_H
#define SURGE_SRC_SURGE_XT_ASSISTANTCLIENT_H

#include "juce_core/juce_core.h"

#include <future>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace Surge
{
namespace Assistant
{

enum class Provider
{
    None,
    ChatGPT,
    DeepSeek,
    MiniMax,
};

juce::String providerId(Provider provider);
juce::String providerDisplayName(Provider provider);
juce::String providerDefaultModel(Provider provider);
juce::String providerKeyPage(Provider provider);
juce::String modelDisplayName(Provider provider, const juce::String &model);
Provider providerFromId(const juce::String &id);

struct ParameterOption
{
    float value{0.0f};
    juce::String label;
};

struct ParameterInfo
{
    int id{-1};
    juce::String name;
    juce::String display;
    float currentValue{0.0f};
    float defaultValue{0.0f};
    std::vector<ParameterOption> options;
};

struct PatchRequest
{
    Provider provider{Provider::None};
    juce::String model;
    juce::String prompt;
    std::vector<juce::String> previousPrompts;
    juce::String patchName;
    bool freshPatch{false};
    std::vector<ParameterInfo> parameters;
};

struct PatchOperation
{
    int parameterId{-1};
    float value{0.0f};
};

struct PatchPlan
{
    juce::String name;
    juce::String summary;
    std::vector<PatchOperation> operations;
};

enum class ResultKind
{
    Connection,
    Generation,
};

struct Result
{
    ResultKind kind{ResultKind::Generation};
    bool ok{false};
    bool credentialPersistent{false};
    juce::String error;
    std::vector<juce::String> models;
    PatchPlan plan;
};

Result parsePatchPlanJson(const juce::String &json, bool freshPatch,
                          const std::set<int> &allowedParameterIds);
Result parseChatCompletion(const juce::String &response, bool freshPatch,
                           const std::set<int> &allowedParameterIds);

class Client
{
  public:
    Client();
    ~Client();

    std::future<Result> connect(Provider provider, const juce::String &apiKey);
    std::future<Result> disconnect(Provider provider);
    std::future<Result> generate(PatchRequest request);
    void cancel();

  private:
    class Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Client)
};

} // namespace Assistant
} // namespace Surge

#endif // SURGE_SRC_SURGE_XT_ASSISTANTCLIENT_H
