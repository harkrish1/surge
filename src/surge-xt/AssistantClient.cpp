/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later).
 */

#include "AssistantClient.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#if JUCE_MAC
#include <Security/Security.h>
#elif JUCE_WINDOWS
#include <windows.h>
#include <wincred.h>
#endif

namespace Surge
{
namespace Assistant
{
namespace
{

constexpr auto credentialService = "org.surge-synth-team.surge-xt.assistant";
constexpr int maximumResponseBytes = 2 * 1024 * 1024;

juce::String providerBaseUrl(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return {};
    case Provider::DeepSeek:
        return "https://api.deepseek.com";
    case Provider::MiniMax:
        return "https://api.minimax.io/v1";
    case Provider::None:
        return {};
    }
    return {};
}

juce::String providerEnvironmentVariable(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return {};
    case Provider::DeepSeek:
        return "DEEPSEEK_API_KEY";
    case Provider::MiniMax:
        return "MINIMAX_API_KEY";
    case Provider::None:
        return {};
    }
    return {};
}

juce::String credentialAccount(Provider provider) { return "provider." + providerId(provider); }

struct CredentialResult
{
    bool ok{false};
    bool persistent{false};
    juce::String secret;
    juce::String error;
};

std::mutex fallbackCredentialMutex;
std::map<Provider, juce::String> fallbackCredentials;

#if JUCE_MAC
CFStringRef makeCFString(const juce::String &value)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, value.toRawUTF8(), kCFStringEncodingUTF8);
}

juce::String securityError(OSStatus status)
{
    auto message = SecCopyErrorMessageString(status, nullptr);
    if (message == nullptr)
        return "OSStatus " + juce::String(static_cast<int>(status));

    std::array<char, 512> buffer{};
    auto converted =
        CFStringGetCString(message, buffer.data(), buffer.size(), kCFStringEncodingUTF8);
    CFRelease(message);
    return converted ? juce::String::fromUTF8(buffer.data())
                     : "OSStatus " + juce::String(static_cast<int>(status));
}

CFMutableDictionaryRef makeCredentialQuery(Provider provider)
{
    auto query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    auto service = makeCFString(credentialService);
    auto account = makeCFString(credentialAccount(provider));
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return query;
}
#endif

CredentialResult readCredential(Provider provider)
{
    if (provider == Provider::None)
        return {false, false, {}, "No assistant provider is selected."};

    auto environmentName = providerEnvironmentVariable(provider);
    if (environmentName.isNotEmpty())
    {
        if (auto value = std::getenv(environmentName.toRawUTF8()))
        {
            auto key = juce::String::fromUTF8(value).trim();
            if (key.isNotEmpty())
                return {true, false, key, {}};
        }
    }

#if JUCE_MAC
    auto query = makeCredentialQuery(provider);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    auto status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound)
        return {false, true, {}, "No API key is stored for " + providerDisplayName(provider) + "."};
    if (status != errSecSuccess || result == nullptr || CFGetTypeID(result) != CFDataGetTypeID())
    {
        if (result != nullptr)
            CFRelease(result);
        return {false, true, {}, "Keychain read failed: " + securityError(status)};
    }

    auto data = static_cast<CFDataRef>(result);
    auto key = juce::String::fromUTF8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                      static_cast<int>(CFDataGetLength(data)));
    CFRelease(result);
    return key.isNotEmpty() ? CredentialResult{true, true, key, {}}
                            : CredentialResult{false, true, {}, "The stored API key is empty."};
#elif JUCE_WINDOWS
    auto target = juce::String(credentialService) + "." + credentialAccount(provider);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.toWideCharPointer(), CRED_TYPE_GENERIC, 0, &credential))
    {
        auto code = GetLastError();
        if (code == ERROR_NOT_FOUND)
            return {
                false, true, {}, "No API key is stored for " + providerDisplayName(provider) + "."};
        return {false, true, {}, "Credential Manager read failed: " + juce::String(code)};
    }

    auto key = juce::String::fromUTF8(reinterpret_cast<const char *>(credential->CredentialBlob),
                                      static_cast<int>(credential->CredentialBlobSize));
    CredFree(credential);
    return key.isNotEmpty() ? CredentialResult{true, true, key, {}}
                            : CredentialResult{false, true, {}, "The stored API key is empty."};
#else
    std::lock_guard<std::mutex> guard(fallbackCredentialMutex);
    auto found = fallbackCredentials.find(provider);
    if (found == fallbackCredentials.end())
        return {false,
                false,
                {},
                "No session API key is stored for " + providerDisplayName(provider) + "."};
    return {true, false, found->second, {}};
#endif
}

CredentialResult writeCredential(Provider provider, const juce::String &secret)
{
    auto key = secret.trim();
    if (provider == Provider::None || key.isEmpty())
        return {false, false, {}, "Enter a non-empty API key."};
    if (key.containsAnyOf("\r\n"))
        return {false, false, {}, "The API key contains an invalid line break."};

#if JUCE_MAC
    auto bytes = key.toRawUTF8();
    auto data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes),
                             static_cast<CFIndex>(std::strlen(bytes)));
    auto query = makeCredentialQuery(provider);
    auto attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kSecValueData, data);
    auto status = SecItemUpdate(query, attributes);
    if (status == errSecItemNotFound)
    {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(attributes);
    CFRelease(query);
    CFRelease(data);
    if (status != errSecSuccess)
        return {false, true, {}, "Keychain write failed: " + securityError(status)};
    return {true, true, {}, {}};
#elif JUCE_WINDOWS
    auto target = juce::String(credentialService) + "." + credentialAccount(provider);
    auto utf8 = key.toUTF8();
    if (utf8.sizeInBytes() - 1 > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return {false, true, {}, "The API key is too large for Credential Manager."};

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.toWideCharPointer());
    credential.CredentialBlobSize = static_cast<DWORD>(utf8.sizeInBytes() - 1);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(utf8.getAddress()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"Surge XT");
    if (!CredWriteW(&credential, 0))
        return {
            false, true, {}, "Credential Manager write failed: " + juce::String(GetLastError())};
    return {true, true, {}, {}};
#else
    std::lock_guard<std::mutex> guard(fallbackCredentialMutex);
    fallbackCredentials[provider] = key;
    return {true, false, {}, {}};
#endif
}

CredentialResult eraseCredential(Provider provider)
{
    if (provider == Provider::None)
        return {true, false, {}, {}};

#if JUCE_MAC
    auto query = makeCredentialQuery(provider);
    auto status = SecItemDelete(query);
    CFRelease(query);
    if (status != errSecSuccess && status != errSecItemNotFound)
        return {false, true, {}, "Keychain delete failed: " + securityError(status)};
    return {true, true, {}, {}};
#elif JUCE_WINDOWS
    auto target = juce::String(credentialService) + "." + credentialAccount(provider);
    if (!CredDeleteW(target.toWideCharPointer(), CRED_TYPE_GENERIC, 0) &&
        GetLastError() != ERROR_NOT_FOUND)
        return {
            false, true, {}, "Credential Manager delete failed: " + juce::String(GetLastError())};
    return {true, true, {}, {}};
#else
    std::lock_guard<std::mutex> guard(fallbackCredentialMutex);
    fallbackCredentials.erase(provider);
    return {true, false, {}, {}};
#endif
}

juce::String sanitizedText(juce::String text, int maximumLength)
{
    text = text.replaceCharacters("\r\n\t", "   ").trim();
    return text.substring(0, maximumLength);
}

juce::String extractJsonObject(juce::String text)
{
    text = text.trim();
    auto thinkEnd = text.lastIndexOfIgnoreCase("</think>");
    if (thinkEnd >= 0)
        text = text.substring(thinkEnd + 8).trim();

    if (text.startsWith("```"))
    {
        auto firstNewline = text.indexOfChar('\n');
        if (firstNewline >= 0)
            text = text.substring(firstNewline + 1);
        auto closingFence = text.lastIndexOf("```");
        if (closingFence >= 0)
            text = text.substring(0, closingFence);
        text = text.trim();
    }

    auto firstBrace = text.indexOfChar('{');
    auto lastBrace = text.lastIndexOfChar('}');
    if (firstBrace >= 0 && lastBrace > firstBrace)
        return text.substring(firstBrace, lastBrace + 1);
    return text;
}

juce::String providerErrorFromBody(const juce::String &body)
{
    juce::var root;
    if (juce::JSON::parse(body, root).wasOk())
    {
        auto error = root.getProperty("error", {});
        if (auto object = error.getDynamicObject())
        {
            auto message = object->getProperty("message").toString();
            if (message.isNotEmpty())
                return sanitizedText(message, 300);
        }
        if (error.isString())
            return sanitizedText(error.toString(), 300);
        auto message = root.getProperty("message", {}).toString();
        if (message.isNotEmpty())
            return sanitizedText(message, 300);
    }
    return sanitizedText(body, 300);
}

juce::String buildSystemPrompt(bool freshPatch)
{
    auto modeInstruction =
        freshPatch ? "The current patch is an untouched init patch. Create a complete, "
                     "musically useful sound and make sure at least one oscillator is audible."
                   : "This is a follow-up request. Preserve the current sound and change only "
                     "the smallest set of parameters needed for the request.";

    return juce::String(
               "You are the Surge XT patch-planning engine. Return exactly one JSON object and no "
               "Markdown or commentary. You may only change parameters listed in the supplied "
               "catalog. "
               "Every value is normalized from 0.0 to 1.0. For discrete parameters, use one of the "
               "exact "
               "option values supplied. Never invent parameter IDs. Unmentioned parameters remain "
               "unchanged. "
               "If you change an oscillator or effect type, leave its type-specific parameters at "
               "their defaults in this plan. "
               "Do not call tools, inspect files, or run commands; solve the patch request only "
               "from the supplied catalog. "
               "The schema is {\"name\":\"short patch name or empty for a follow-up\","
               "\"summary\":\"short user-facing description\",\"operations\":[{"
               "\"parameter_id\":123,\"value\":0.5}]}. ") +
           modeInstruction;
}

juce::String buildUserPrompt(const PatchRequest &request)
{
    juce::String conversationContext;
    if (!request.previousPrompts.empty())
    {
        constexpr std::size_t maximumPreviousPrompts = 9;
        auto firstPrompt = request.previousPrompts.size() > maximumPreviousPrompts
                               ? request.previousPrompts.size() - maximumPreviousPrompts
                               : 0;
        juce::Array<juce::var> previousPrompts;
        for (auto index = firstPrompt; index < request.previousPrompts.size(); ++index)
            previousPrompts.add(sanitizedText(request.previousPrompts[index], 2000));
        conversationContext = "Previous user requests, oldest to newest (context only):\n" +
                              juce::JSON::toString(juce::var(previousPrompts), true) + "\n";
    }

    juce::Array<juce::var> catalog;
    catalog.ensureStorageAllocated(static_cast<int>(request.parameters.size()));
    for (const auto &parameter : request.parameters)
    {
        auto item = new juce::DynamicObject();
        item->setProperty("id", parameter.id);
        item->setProperty("name", parameter.name);
        item->setProperty("current", parameter.currentValue);
        item->setProperty("default", parameter.defaultValue);
        item->setProperty("display", parameter.display);
        if (!parameter.options.empty())
        {
            juce::Array<juce::var> options;
            for (const auto &option : parameter.options)
            {
                auto optionObject = new juce::DynamicObject();
                optionObject->setProperty("value", option.value);
                optionObject->setProperty("label", option.label);
                options.add(juce::var(optionObject));
            }
            item->setProperty("options", juce::var(options));
        }
        catalog.add(juce::var(item));
    }

    return conversationContext +
           "Current user request (perform this now): " + sanitizedText(request.prompt, 2000) +
           "\nCurrent patch name: " + request.patchName + "\nAllowed parameter catalog (JSON):\n" +
           juce::JSON::toString(juce::var(catalog), true);
}

juce::String buildChatRequest(const PatchRequest &request)
{
    juce::Array<juce::var> messages;
    auto system = new juce::DynamicObject();
    system->setProperty("role", "system");
    system->setProperty("content", buildSystemPrompt(request.freshPatch));
    messages.add(juce::var(system));

    auto user = new juce::DynamicObject();
    user->setProperty("role", "user");
    user->setProperty("content", buildUserPrompt(request));
    messages.add(juce::var(user));

    auto responseFormat = new juce::DynamicObject();
    responseFormat->setProperty("type", "json_object");

    auto root = new juce::DynamicObject();
    root->setProperty("model", request.model);
    root->setProperty("messages", juce::var(messages));
    root->setProperty("stream", false);
    root->setProperty("temperature", request.freshPatch ? 0.35 : 0.15);
    root->setProperty("max_tokens", request.freshPatch ? 4096 : 2048);
    root->setProperty("response_format", juce::var(responseFormat));
    if (request.provider == Provider::MiniMax)
        root->setProperty("reasoning_split", true);
    return juce::JSON::toString(juce::var(root), true);
}

juce::String buildPatchPlanSchema(const PatchRequest &request)
{
    juce::Array<juce::var> parameterIds;
    for (const auto &parameter : request.parameters)
        parameterIds.add(parameter.id);

    auto idProperty = new juce::DynamicObject();
    idProperty->setProperty("type", "integer");
    idProperty->setProperty("enum", juce::var(parameterIds));

    auto valueProperty = new juce::DynamicObject();
    valueProperty->setProperty("type", "number");
    valueProperty->setProperty("minimum", 0.0);
    valueProperty->setProperty("maximum", 1.0);

    auto operationProperties = new juce::DynamicObject();
    operationProperties->setProperty("parameter_id", juce::var(idProperty));
    operationProperties->setProperty("value", juce::var(valueProperty));

    juce::Array<juce::var> operationRequired{"parameter_id", "value"};
    auto operation = new juce::DynamicObject();
    operation->setProperty("type", "object");
    operation->setProperty("properties", juce::var(operationProperties));
    operation->setProperty("required", juce::var(operationRequired));
    operation->setProperty("additionalProperties", false);

    auto name = new juce::DynamicObject();
    name->setProperty("type", "string");
    name->setProperty("maxLength", 64);
    auto summary = new juce::DynamicObject();
    summary->setProperty("type", "string");
    summary->setProperty("maxLength", 180);
    auto operations = new juce::DynamicObject();
    operations->setProperty("type", "array");
    operations->setProperty("items", juce::var(operation));
    operations->setProperty("minItems", 1);
    operations->setProperty("maxItems", request.freshPatch ? 128 : 32);

    auto properties = new juce::DynamicObject();
    properties->setProperty("name", juce::var(name));
    properties->setProperty("summary", juce::var(summary));
    properties->setProperty("operations", juce::var(operations));

    juce::Array<juce::var> required{"name", "summary", "operations"};
    auto root = new juce::DynamicObject();
    root->setProperty("type", "object");
    root->setProperty("properties", juce::var(properties));
    root->setProperty("required", juce::var(required));
    root->setProperty("additionalProperties", false);
    return juce::JSON::toString(juce::var(root), true);
}

juce::File findCodexExecutable()
{
    std::vector<juce::File> candidates;
    if (auto configured = std::getenv("SURGE_XT_CODEX_PATH"))
        candidates.emplace_back(juce::String::fromUTF8(configured));

    auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    candidates.push_back(home.getChildFile(".local/bin/codex"));
    candidates.push_back(home.getChildFile(".cargo/bin/codex"));
    candidates.push_back(home.getChildFile("Library/Application Support/Surge XT/codex"));
    candidates.emplace_back("/opt/homebrew/bin/codex");
    candidates.emplace_back("/usr/local/bin/codex");
    candidates.emplace_back("/usr/bin/codex");

    auto found = std::find_if(candidates.begin(), candidates.end(),
                              [](const auto &candidate) { return candidate.existsAsFile(); });
    return found != candidates.end() ? *found : juce::File{};
}

juce::String buildCodexPrompt(const PatchRequest &request)
{
    return buildSystemPrompt(request.freshPatch) + "\n" + buildUserPrompt(request);
}

struct ScopedDirectoryRemoval
{
    explicit ScopedDirectoryRemoval(juce::File directory) : directory(std::move(directory)) {}
    ~ScopedDirectoryRemoval() { directory.deleteRecursively(); }
    juce::File directory;
};

std::vector<juce::String> parseModelList(const juce::String &body, juce::String &error)
{
    juce::var root;
    auto parsed = juce::JSON::parse(body, root);
    if (parsed.failed())
    {
        error = "The provider returned invalid model-list JSON: " + parsed.getErrorMessage();
        return {};
    }

    auto data = root.getProperty("data", {});
    auto array = data.getArray();
    if (array == nullptr)
    {
        error = "The provider response did not contain a model list.";
        return {};
    }

    std::set<juce::String> unique;
    for (const auto &entry : *array)
    {
        auto id = entry.getDynamicObject() != nullptr
                      ? entry.getDynamicObject()->getProperty("id").toString().trim()
                      : entry.toString().trim();
        if (id.isNotEmpty() && id.length() <= 160)
            unique.insert(id);
        if (unique.size() >= 300)
            break;
    }

    if (unique.empty())
    {
        error = "The connected account did not return any available models.";
        return {};
    }
    return {unique.begin(), unique.end()};
}

} // namespace

juce::String providerId(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return "chatgpt";
    case Provider::DeepSeek:
        return "deepseek";
    case Provider::MiniMax:
        return "minimax";
    case Provider::None:
        return "none";
    }
    return "none";
}

juce::String providerDisplayName(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return "ChatGPT Plus";
    case Provider::DeepSeek:
        return "DeepSeek";
    case Provider::MiniMax:
        return "MiniMax";
    case Provider::None:
        return "Local fallback";
    }
    return "Local fallback";
}

juce::String providerDefaultModel(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return "default";
    case Provider::DeepSeek:
        return "deepseek-v4-flash";
    case Provider::MiniMax:
        return "MiniMax-M3";
    case Provider::None:
        return {};
    }
    return {};
}

juce::String providerKeyPage(Provider provider)
{
    switch (provider)
    {
    case Provider::ChatGPT:
        return "https://developers.openai.com/codex/cli";
    case Provider::DeepSeek:
        return "https://platform.deepseek.com/api_keys";
    case Provider::MiniMax:
        return "https://platform.minimax.io/login";
    case Provider::None:
        return {};
    }
    return {};
}

Provider providerFromId(const juce::String &id)
{
    if (id.equalsIgnoreCase("chatgpt"))
        return Provider::ChatGPT;
    if (id.equalsIgnoreCase("deepseek"))
        return Provider::DeepSeek;
    if (id.equalsIgnoreCase("minimax"))
        return Provider::MiniMax;
    return Provider::None;
}

juce::String modelDisplayName(Provider provider, const juce::String &model)
{
    return provider == Provider::ChatGPT && model == "default" ? juce::String("Codex default")
                                                               : model;
}

Result parsePatchPlanJson(const juce::String &json, bool freshPatch,
                          const std::set<int> &allowedParameterIds)
{
    Result result;
    result.kind = ResultKind::Generation;

    juce::var root;
    auto parsed = juce::JSON::parse(extractJsonObject(json), root);
    if (parsed.failed() || root.getDynamicObject() == nullptr)
    {
        result.error = "The model did not return a valid patch plan: " + parsed.getErrorMessage();
        return result;
    }

    auto name = root.getProperty("name", {}).toString();
    auto summary = root.getProperty("summary", {}).toString();
    auto operations = root.getProperty("operations", {}).getArray();
    if (operations == nullptr)
    {
        result.error = "The model response did not contain a patch operation list.";
        return result;
    }

    auto maximumOperations = freshPatch ? 128 : 32;
    if (operations->isEmpty() || operations->size() > maximumOperations)
    {
        result.error = operations->isEmpty()
                           ? "The model did not propose any patch changes."
                           : "The model proposed too many changes for this request.";
        return result;
    }

    std::set<int> seenIds;
    for (const auto &operationValue : *operations)
    {
        auto operation = operationValue.getDynamicObject();
        if (operation == nullptr)
        {
            result.error = "A patch operation was not a JSON object.";
            return result;
        }

        auto idValue = operation->getProperty("parameter_id");
        auto valueValue = operation->getProperty("value");
        if (!(idValue.isInt() || idValue.isInt64() || idValue.isDouble()) ||
            !(valueValue.isInt() || valueValue.isInt64() || valueValue.isDouble()))
        {
            result.error = "A patch operation contained an invalid parameter ID or value.";
            return result;
        }

        auto idAsDouble = static_cast<double>(idValue);
        auto valueAsDouble = static_cast<double>(valueValue);
        if (!std::isfinite(idAsDouble) || std::floor(idAsDouble) != idAsDouble ||
            idAsDouble < 0.0 || idAsDouble > static_cast<double>(std::numeric_limits<int>::max()) ||
            !std::isfinite(valueAsDouble))
        {
            result.error = "A patch operation contained an invalid parameter ID or value.";
            return result;
        }

        auto id = static_cast<int>(idAsDouble);
        auto value = static_cast<float>(valueAsDouble);
        if (allowedParameterIds.find(id) == allowedParameterIds.end())
        {
            result.error = "The model attempted to change an unknown parameter.";
            return result;
        }
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        {
            result.error = "The model returned a parameter value outside the valid range.";
            return result;
        }
        if (!seenIds.insert(id).second)
        {
            result.error = "The model returned the same parameter more than once.";
            return result;
        }
        result.plan.operations.push_back({id, value});
    }

    result.plan.name = sanitizedText(name, 64);
    result.plan.summary = sanitizedText(summary, 180);
    if (!freshPatch)
        result.plan.name.clear();
    if (result.plan.summary.isEmpty())
        result.plan.summary =
            freshPatch ? "Created a new assistant patch." : "Updated the current patch.";
    result.ok = true;
    return result;
}

Result parseChatCompletion(const juce::String &response, bool freshPatch,
                           const std::set<int> &allowedParameterIds)
{
    Result result;
    result.kind = ResultKind::Generation;

    juce::var root;
    auto parsed = juce::JSON::parse(response, root);
    if (parsed.failed())
    {
        result.error = "The provider returned invalid JSON: " + parsed.getErrorMessage();
        return result;
    }

    auto choices = root.getProperty("choices", {}).getArray();
    if (choices == nullptr || choices->isEmpty())
    {
        result.error = "The provider response did not contain a model answer.";
        return result;
    }

    auto choice = choices->getReference(0).getDynamicObject();
    auto message = choice != nullptr ? choice->getProperty("message").getDynamicObject() : nullptr;
    auto content = message != nullptr ? message->getProperty("content").toString() : juce::String{};
    if (content.isEmpty())
    {
        result.error = "The provider returned an empty model answer.";
        return result;
    }
    return parsePatchPlanJson(content, freshPatch, allowedParameterIds);
}

class Client::Impl
{
  public:
    Impl() : worker([this]() { run(); }) {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> guard(queueMutex);
            stopping = true;
            while (!jobs.empty())
            {
                auto result = failure(jobs.front()->kind, "The assistant request was cancelled.");
                jobs.front()->promise.set_value(std::move(result));
                jobs.pop_front();
            }
        }
        cancelActiveRequest();
        cancelActiveProcess();
        queueCondition.notify_all();
        if (worker.joinable())
            worker.join();
    }

    std::future<Result> connect(Provider provider, const juce::String &key)
    {
        auto job = std::make_unique<Job>();
        job->type = JobType::Connect;
        job->kind = ResultKind::Connection;
        job->provider = provider;
        job->key = key;
        return enqueue(std::move(job));
    }

    std::future<Result> disconnect(Provider provider)
    {
        auto job = std::make_unique<Job>();
        job->type = JobType::Disconnect;
        job->kind = ResultKind::Connection;
        job->provider = provider;
        return enqueue(std::move(job));
    }

    std::future<Result> generate(PatchRequest request)
    {
        auto job = std::make_unique<Job>();
        job->type = JobType::Generate;
        job->kind = ResultKind::Generation;
        job->provider = request.provider;
        job->request = std::move(request);
        return enqueue(std::move(job));
    }

    void cancel()
    {
        {
            std::lock_guard<std::mutex> guard(queueMutex);
            while (!jobs.empty())
            {
                auto result = failure(jobs.front()->kind, "The assistant request was cancelled.");
                jobs.front()->promise.set_value(std::move(result));
                jobs.pop_front();
            }
        }
        cancelActiveRequest();
        cancelActiveProcess();
    }

  private:
    enum class JobType
    {
        Connect,
        Disconnect,
        Generate,
    };

    struct Job
    {
        JobType type{JobType::Generate};
        ResultKind kind{ResultKind::Generation};
        Provider provider{Provider::None};
        juce::String key;
        PatchRequest request;
        std::promise<Result> promise;
    };

    struct HttpResponse
    {
        int statusCode{0};
        juce::String body;
        juce::String error;
    };

    struct ProcessResponse
    {
        uint32_t exitCode{1};
        juce::String output;
        juce::String error;
    };

    static Result failure(ResultKind kind, const juce::String &error)
    {
        Result result;
        result.kind = kind;
        result.error = error;
        return result;
    }

    std::future<Result> enqueue(std::unique_ptr<Job> job)
    {
        auto future = job->promise.get_future();
        {
            std::lock_guard<std::mutex> guard(queueMutex);
            if (stopping)
            {
                job->promise.set_value(
                    failure(job->kind, "The assistant service is shutting down."));
                return future;
            }
            jobs.push_back(std::move(job));
        }
        queueCondition.notify_one();
        return future;
    }

    void cancelActiveRequest()
    {
        std::lock_guard<std::mutex> guard(streamMutex);
        if (activeStream != nullptr)
            activeStream->cancel();
    }

    void cancelActiveProcess()
    {
        std::lock_guard<std::mutex> guard(processMutex);
        if (activeProcess != nullptr)
            activeProcess->kill();
    }

    ProcessResponse runProcess(const juce::StringArray &arguments)
    {
        ProcessResponse response;
        auto process = std::make_unique<juce::ChildProcess>();
        if (!process->start(arguments))
        {
            response.error = "Could not start the Codex runtime.";
            response.output = response.error;
            return response;
        }

        {
            std::lock_guard<std::mutex> guard(processMutex);
            activeProcess = process.get();
        }

        juce::MemoryOutputStream output;
        std::array<char, 4096> buffer{};
        for (;;)
        {
            auto bytesRead =
                process->readProcessOutput(buffer.data(), static_cast<int>(buffer.size()));
            if (bytesRead <= 0)
                break;
            if (output.getDataSize() < maximumResponseBytes)
            {
                auto remaining = maximumResponseBytes - static_cast<int>(output.getDataSize());
                output.write(buffer.data(), static_cast<size_t>(std::min(bytesRead, remaining)));
            }
        }
        process->waitForProcessToFinish(1000);
        response.exitCode = process->getExitCode();
        response.output = output.toString();

        {
            std::lock_guard<std::mutex> guard(processMutex);
            activeProcess = nullptr;
        }
        return response;
    }

    HttpResponse performHttp(const juce::String &url, const juce::String &method,
                             const juce::String &key, const juce::String &body = {})
    {
        HttpResponse response;
        auto requestUrl = juce::URL(url);
        if (method == "POST")
            requestUrl = requestUrl.withPOSTData(body);

        auto stream = std::make_unique<juce::WebInputStream>(requestUrl, method == "POST");
        stream->withCustomRequestCommand(method)
            .withConnectionTimeout(30000)
            .withNumRedirectsToFollow(2)
            .withExtraHeaders("Authorization: Bearer " + key +
                              "\r\nAccept: application/json\r\nContent-Type: application/json\r\n"
                              "User-Agent: Surge-XT-Assistant\r\n");

        {
            std::lock_guard<std::mutex> guard(streamMutex);
            activeStream = stream.get();
        }

        auto connected = stream->connect(nullptr);
        response.statusCode = stream->getStatusCode();
        if (!connected)
        {
            response.error = "Could not connect to the provider.";
        }
        else
        {
            juce::MemoryOutputStream output;
            std::array<char, 8192> buffer{};
            while (!stream->isExhausted() && output.getDataSize() <= maximumResponseBytes)
            {
                auto read = stream->read(buffer.data(), static_cast<int>(buffer.size()));
                if (read <= 0)
                    break;
                output.write(buffer.data(), static_cast<size_t>(read));
            }
            if (output.getDataSize() > maximumResponseBytes)
                response.error = "The provider response was unexpectedly large.";
            else
                response.body = output.toString();
        }

        {
            std::lock_guard<std::mutex> guard(streamMutex);
            activeStream = nullptr;
        }
        return response;
    }

    Result processConnect(const Job &job)
    {
        Result result;
        result.kind = ResultKind::Connection;

        if (job.provider == Provider::ChatGPT)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
            {
                result.error =
                    "Install the official Codex CLI first, then try ChatGPT sign-in again.";
                return result;
            }

            auto status = runProcess({codex.getFullPathName(), "login", "status"});
            if (status.exitCode != 0 || !status.output.containsIgnoreCase("ChatGPT"))
            {
                auto login = runProcess({codex.getFullPathName(), "login"});
                if (login.exitCode != 0)
                {
                    result.error = "ChatGPT sign-in failed: " + sanitizedText(login.output, 300);
                    return result;
                }
                status = runProcess({codex.getFullPathName(), "login", "status"});
            }

            if (status.exitCode != 0 || !status.output.containsIgnoreCase("ChatGPT"))
            {
                result.error = "Codex did not report an active ChatGPT subscription login.";
                return result;
            }
            result.models = {providerDefaultModel(Provider::ChatGPT)};
            result.credentialPersistent = true;
            result.ok = true;
            return result;
        }

        auto key = job.key.trim();
        if (job.provider == Provider::None || key.isEmpty() || key.containsAnyOf("\r\n"))
        {
            result.error = "Enter a valid API key.";
            return result;
        }

        auto response = performHttp(providerBaseUrl(job.provider) + "/models", "GET", key);
        if (response.error.isNotEmpty())
        {
            result.error = response.error;
            return result;
        }
        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            result.error = providerDisplayName(job.provider) + " rejected the connection (HTTP " +
                           juce::String(response.statusCode) +
                           "): " + providerErrorFromBody(response.body);
            return result;
        }

        juce::String modelError;
        result.models = parseModelList(response.body, modelError);
        result.models.erase(std::remove_if(result.models.begin(), result.models.end(),
                                           [provider = job.provider](auto model) {
                                               return provider == Provider::DeepSeek
                                                          ? !model.startsWithIgnoreCase("deepseek-")
                                                          : !model.startsWithIgnoreCase(
                                                                "minimax-m");
                                           }),
                            result.models.end());
        if (result.models.empty())
        {
            result.error = modelError.isNotEmpty()
                               ? modelError
                               : "The connected account did not return a supported text model.";
            return result;
        }

        auto stored = writeCredential(job.provider, key);
        if (!stored.ok)
        {
            result.error = stored.error;
            return result;
        }
        result.credentialPersistent = stored.persistent;
        result.ok = true;
        return result;
    }

    Result processDisconnect(const Job &job)
    {
        Result result;
        result.kind = ResultKind::Connection;

        if (job.provider == Provider::ChatGPT)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
            {
                result.error = "The Codex runtime could not be found.";
                return result;
            }
            auto logout = runProcess({codex.getFullPathName(), "logout"});
            result.ok = logout.exitCode == 0;
            if (!result.ok)
                result.error = "ChatGPT sign-out failed: " + sanitizedText(logout.output, 300);
            result.credentialPersistent = true;
            return result;
        }

        auto erased = eraseCredential(job.provider);
        result.ok = erased.ok;
        result.credentialPersistent = erased.persistent;
        result.error = erased.error;
        return result;
    }

    Result processGenerate(const Job &job)
    {
        if (job.request.provider == Provider::None || job.request.model.isEmpty())
            return failure(ResultKind::Generation,
                           "Select and configure an assistant connection first.");

        if (job.request.provider == Provider::ChatGPT)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
                return failure(ResultKind::Generation,
                               "The official Codex CLI is required for ChatGPT Plus.");

            auto temporaryRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("surge-xt-assistant-" + juce::Uuid().toString());
            if (temporaryRoot.createDirectory().failed())
                return failure(ResultKind::Generation,
                               "Could not create a private temporary assistant directory.");
            ScopedDirectoryRemoval cleanup(temporaryRoot);

            auto promptFile = temporaryRoot.getChildFile("prompt.txt");
            auto schemaFile = temporaryRoot.getChildFile("patch-plan-schema.json");
            auto outputFile = temporaryRoot.getChildFile("patch-plan.json");
            if (!promptFile.replaceWithText(buildCodexPrompt(job.request)) ||
                !schemaFile.replaceWithText(buildPatchPlanSchema(job.request)))
                return failure(ResultKind::Generation,
                               "Could not prepare the temporary Codex request files.");

#if JUCE_WINDOWS
            return failure(ResultKind::Generation,
                           "ChatGPT Plus through Codex is not yet available in the Windows build.");
#else
            juce::String script =
                "exec \"$1\" exec --ephemeral --ignore-user-config --ignore-rules "
                "--skip-git-repo-check --sandbox read-only --color never "
                "-c 'approval_policy=\"never\"' -c 'web_search=\"disabled\"' "
                "--disable shell_tool --disable unified_exec --disable apps "
                "--disable browser_use --disable computer_use --disable image_generation "
                "--disable multi_agent --disable hooks --disable plugins "
                "-C \"$5\" --output-schema \"$3\" --output-last-message \"$4\" ";
            if (job.request.model != "default")
                script += "--model \"$6\" ";
            script += "- < \"$2\"";

            juce::StringArray arguments{"/bin/sh",
                                        "-c",
                                        script,
                                        "surge-xt-codex",
                                        codex.getFullPathName(),
                                        promptFile.getFullPathName(),
                                        schemaFile.getFullPathName(),
                                        outputFile.getFullPathName(),
                                        temporaryRoot.getFullPathName(),
                                        job.request.model};
            auto execution = runProcess(arguments);
            if (execution.exitCode != 0)
                return failure(ResultKind::Generation,
                               "Codex generation failed: " + sanitizedText(execution.output, 400));
            if (!outputFile.existsAsFile())
                return failure(ResultKind::Generation,
                               "Codex completed without returning a patch plan.");

            std::set<int> allowedIds;
            for (const auto &parameter : job.request.parameters)
                allowedIds.insert(parameter.id);
            return parsePatchPlanJson(outputFile.loadFileAsString(), job.request.freshPatch,
                                      allowedIds);
#endif
        }

        auto credential = readCredential(job.request.provider);
        if (!credential.ok)
            return failure(ResultKind::Generation, credential.error);

        auto requestBody = buildChatRequest(job.request);
        auto response = performHttp(providerBaseUrl(job.request.provider) + "/chat/completions",
                                    "POST", credential.secret, requestBody);
        credential.secret.clear();
        if (response.error.isNotEmpty())
            return failure(ResultKind::Generation, response.error);
        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            return failure(ResultKind::Generation, providerDisplayName(job.request.provider) +
                                                       " returned HTTP " +
                                                       juce::String(response.statusCode) + ": " +
                                                       providerErrorFromBody(response.body));
        }

        std::set<int> allowedIds;
        for (const auto &parameter : job.request.parameters)
            allowedIds.insert(parameter.id);
        return parseChatCompletion(response.body, job.request.freshPatch, allowedIds);
    }

    void run()
    {
        for (;;)
        {
            std::unique_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait(lock, [this]() { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty())
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            Result result;
            switch (job->type)
            {
            case JobType::Connect:
                result = processConnect(*job);
                break;
            case JobType::Disconnect:
                result = processDisconnect(*job);
                break;
            case JobType::Generate:
                result = processGenerate(*job);
                break;
            }
            job->key.clear();
            job->promise.set_value(std::move(result));
        }
    }

    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<std::unique_ptr<Job>> jobs;
    bool stopping{false};
    std::thread worker;

    std::mutex streamMutex;
    juce::WebInputStream *activeStream{nullptr};

    std::mutex processMutex;
    juce::ChildProcess *activeProcess{nullptr};
};

Client::Client() : impl(std::make_unique<Impl>()) {}
Client::~Client() = default;

std::future<Result> Client::connect(Provider provider, const juce::String &apiKey)
{
    return impl->connect(provider, apiKey);
}

std::future<Result> Client::disconnect(Provider provider) { return impl->disconnect(provider); }

std::future<Result> Client::generate(PatchRequest request)
{
    return impl->generate(std::move(request));
}

void Client::cancel() { impl->cancel(); }

} // namespace Assistant
} // namespace Surge
