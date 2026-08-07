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
#endif
#if JUCE_WINDOWS
#include <windows.h>
#include <wincred.h>
#else
#include <sys/stat.h>
#endif

namespace Surge
{
namespace Assistant
{
namespace
{

constexpr auto credentialService = "org.surge-synth-team.surge-xt.assistant";
constexpr int maximumResponseBytes = 2 * 1024 * 1024;
constexpr int maximumDiagnosticBytes = 512 * 1024;

enum class ProviderProtocol
{
    Local,
    Codex,
    OpenAIResponses,
    OpenAICompatible,
    Anthropic,
};

struct ProviderConfig
{
    Provider provider;
    const char *id;
    const char *displayName;
    const char *baseUrl;
    const char *environmentVariable;
    const char *defaultModel;
    const char *keyPage;
    ProviderProtocol protocol;
};

constexpr std::array<ProviderConfig, 8> providerConfigs{{
    {Provider::None, "none", "Local fallback", "", "", "", "", ProviderProtocol::Local},
    {Provider::ChatGPT, "chatgpt", "ChatGPT Plus", "", "", "default",
     "https://developers.openai.com/codex/cli", ProviderProtocol::Codex},
    {Provider::OpenAI, "openai", "OpenAI API", "https://api.openai.com/v1", "OPENAI_API_KEY",
     "gpt-5.4-mini", "https://platform.openai.com/api-keys", ProviderProtocol::OpenAIResponses},
    {Provider::Anthropic, "anthropic", "Anthropic", "https://api.anthropic.com",
     "ANTHROPIC_API_KEY", "claude-sonnet-5", "https://console.anthropic.com/settings/keys",
     ProviderProtocol::Anthropic},
    {Provider::Gemini, "gemini", "Google Gemini",
     "https://generativelanguage.googleapis.com/v1beta/openai", "GEMINI_API_KEY",
     "gemini-3.6-flash", "https://aistudio.google.com/app/apikey",
     ProviderProtocol::OpenAICompatible},
    {Provider::OpenRouter, "openrouter", "OpenRouter", "https://openrouter.ai/api/v1",
     "OPENROUTER_API_KEY", "openai/gpt-5.4-mini", "https://openrouter.ai/settings/keys",
     ProviderProtocol::OpenAICompatible},
    {Provider::DeepSeek, "deepseek", "DeepSeek", "https://api.deepseek.com", "DEEPSEEK_API_KEY",
     "deepseek-v4-flash", "https://platform.deepseek.com/api_keys",
     ProviderProtocol::OpenAICompatible},
    {Provider::MiniMax, "minimax", "MiniMax", "https://api.minimax.io/v1", "MINIMAX_API_KEY",
     "MiniMax-M3", "https://platform.minimax.io/login", ProviderProtocol::OpenAICompatible},
}};

const ProviderConfig &configFor(Provider provider)
{
    auto found =
        std::find_if(providerConfigs.begin(), providerConfigs.end(),
                     [provider](const auto &config) { return config.provider == provider; });
    return found != providerConfigs.end() ? *found : providerConfigs.front();
}

juce::String providerBaseUrl(Provider provider) { return configFor(provider).baseUrl; }

juce::String providerEnvironmentVariable(Provider provider)
{
    return configFor(provider).environmentVariable;
}

ProviderProtocol providerProtocol(Provider provider) { return configFor(provider).protocol; }

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
std::mutex diagnosticMutex;

std::mutex &credentialProcessMutex(Provider provider)
{
    static std::array<std::mutex, providerConfigs.size()> mutexes;
    auto index = static_cast<std::size_t>(provider);
    return mutexes[index < mutexes.size() ? index : 0];
}

juce::InterProcessLock &credentialTransactionLock(Provider provider)
{
    static juce::InterProcessLock chatGPT("surge-xt-assistant-credentials-chatgpt");
    static juce::InterProcessLock deepSeek("surge-xt-assistant-credentials-deepseek");
    static juce::InterProcessLock miniMax("surge-xt-assistant-credentials-minimax");
    static juce::InterProcessLock openAI("surge-xt-assistant-credentials-openai");
    static juce::InterProcessLock anthropic("surge-xt-assistant-credentials-anthropic");
    static juce::InterProcessLock gemini("surge-xt-assistant-credentials-gemini");
    static juce::InterProcessLock openRouter("surge-xt-assistant-credentials-openrouter");
    switch (provider)
    {
    case Provider::ChatGPT:
        return chatGPT;
    case Provider::DeepSeek:
        return deepSeek;
    case Provider::MiniMax:
        return miniMax;
    case Provider::OpenAI:
        return openAI;
    case Provider::Anthropic:
        return anthropic;
    case Provider::Gemini:
        return gemini;
    case Provider::OpenRouter:
        return openRouter;
    case Provider::None:
        return openAI;
    }
    return openAI;
}

juce::String diagnosticDetail(juce::String text)
{
    text = text.replaceCharacters("\r\n\t", "   ").trim().substring(0, 240);
    auto providerDetail = text.indexOf(": ");
    if (providerDetail >= 0)
        text = text.substring(0, providerDetail);
    juce::StringArray words;
    words.addTokens(text, " ", {});
    for (auto &word : words)
    {
        if (word.length() > 48 || word.startsWithIgnoreCase("Bearer") ||
            word.startsWithIgnoreCase("sk-") || word.startsWithIgnoreCase("AIza"))
            word = "[redacted]";
    }
    return words.joinIntoString(" ");
}

void writeDiagnostic(const juce::String &action, Provider provider, const juce::String &outcome,
                     double elapsedMilliseconds = 0.0, const juce::String &detail = {})
{
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("Surge Synth Team")
                         .getChildFile("Surge XT");
    if (directory.createDirectory().failed())
        return;

    auto file = directory.getChildFile("AssistantDiagnostics.log");
    if (file.existsAsFile() && file.getSize() > maximumDiagnosticBytes)
    {
        auto previous = directory.getChildFile("AssistantDiagnostics.previous.log");
        previous.deleteFile();
        file.moveFileTo(previous);
    }

    auto line = juce::Time::getCurrentTime().toISO8601(true) + " action=" + action +
                " provider=" + providerId(provider) + " outcome=" + outcome;
    if (elapsedMilliseconds > 0.0)
        line += " elapsed_ms=" + juce::String(juce::roundToInt(elapsedMilliseconds));
    if (detail.isNotEmpty())
        line += " detail=\"" + diagnosticDetail(detail).replaceCharacter('"', '\'') + "\"";
    file.appendText(line + "\n", false, false, "\n");
}

CredentialResult readEnvironmentCredential(Provider provider)
{
    auto environmentName = providerEnvironmentVariable(provider);
    if (environmentName.isEmpty())
        return {};
    if (auto value = std::getenv(environmentName.toRawUTF8()))
    {
        auto key = juce::String::fromUTF8(value).trim();
        if (key.isNotEmpty() && !key.containsAnyOf("\r\n"))
            return {true, false, key, {}};
    }
    return {};
}

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

#if JUCE_MAC
    auto query = makeCredentialQuery(provider);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    auto status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound)
    {
        auto environment = readEnvironmentCredential(provider);
        return environment.ok ? environment
                              : CredentialResult{false,
                                                 true,
                                                 {},
                                                 "No API key is stored for " +
                                                     providerDisplayName(provider) + "."};
    }
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
    return key.isNotEmpty() && !key.containsAnyOf("\r\n")
               ? CredentialResult{true, true, key, {}}
               : CredentialResult{false, true, {}, "The stored API key is invalid."};
#elif JUCE_WINDOWS
    auto target = juce::String(credentialService) + "." + credentialAccount(provider);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.toWideCharPointer(), CRED_TYPE_GENERIC, 0, &credential))
    {
        auto code = GetLastError();
        if (code == ERROR_NOT_FOUND)
        {
            auto environment = readEnvironmentCredential(provider);
            return environment.ok ? environment
                                  : CredentialResult{false,
                                                     true,
                                                     {},
                                                     "No API key is stored for " +
                                                         providerDisplayName(provider) + "."};
        }
        return {false, true, {}, "Credential Manager read failed: " + juce::String(code)};
    }

    auto key = juce::String::fromUTF8(reinterpret_cast<const char *>(credential->CredentialBlob),
                                      static_cast<int>(credential->CredentialBlobSize));
    CredFree(credential);
    return key.isNotEmpty() && !key.containsAnyOf("\r\n")
               ? CredentialResult{true, true, key, {}}
               : CredentialResult{false, true, {}, "The stored API key is invalid."};
#else
    {
        std::lock_guard<std::mutex> guard(fallbackCredentialMutex);
        auto found = fallbackCredentials.find(provider);
        if (found != fallbackCredentials.end())
            return {true, false, found->second, {}};
    }
    auto environment = readEnvironmentCredential(provider);
    return environment.ok ? environment
                          : CredentialResult{false,
                                             false,
                                             {},
                                             "No session API key is stored for " +
                                                 providerDisplayName(provider) + "."};
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

juce::String buildPatchPlanSchema(const PatchRequest &request, bool includeLocalConstraints = true);

juce::var buildStructuredResponseFormat(const PatchRequest &request)
{
    juce::var schema;
    if (juce::JSON::parse(buildPatchPlanSchema(request), schema).failed())
        return {};

    auto jsonSchema = new juce::DynamicObject();
    jsonSchema->setProperty("name", "surge_xt_patch_plan");
    jsonSchema->setProperty("strict", true);
    jsonSchema->setProperty("schema", schema);

    auto responseFormat = new juce::DynamicObject();
    responseFormat->setProperty("type", "json_schema");
    responseFormat->setProperty("json_schema", juce::var(jsonSchema));
    return juce::var(responseFormat);
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

    auto root = new juce::DynamicObject();
    root->setProperty("model", request.model);
    root->setProperty("messages", juce::var(messages));
    root->setProperty("stream", false);
    if (request.provider == Provider::OpenAI)
        root->setProperty("max_completion_tokens", request.freshPatch ? 4096 : 2048);
    else
        root->setProperty("max_tokens", request.freshPatch ? 4096 : 2048);

    if (request.provider == Provider::DeepSeek || request.provider == Provider::MiniMax)
    {
        auto responseFormat = new juce::DynamicObject();
        responseFormat->setProperty("type", "json_object");
        root->setProperty("response_format", juce::var(responseFormat));
        root->setProperty("temperature", request.freshPatch ? 0.35 : 0.15);
    }
    else
    {
        root->setProperty("response_format", buildStructuredResponseFormat(request));
    }

    if (request.provider == Provider::OpenRouter)
    {
        auto preferences = new juce::DynamicObject();
        preferences->setProperty("require_parameters", true);
        root->setProperty("provider", juce::var(preferences));
    }
    if (request.provider == Provider::MiniMax)
        root->setProperty("reasoning_split", true);
    return juce::JSON::toString(juce::var(root), true);
}

juce::String buildOpenAIResponseRequest(const PatchRequest &request)
{
    juce::var schema;
    juce::JSON::parse(buildPatchPlanSchema(request), schema);

    auto format = new juce::DynamicObject();
    format->setProperty("type", "json_schema");
    format->setProperty("name", "surge_xt_patch_plan");
    format->setProperty("strict", true);
    format->setProperty("schema", schema);
    auto text = new juce::DynamicObject();
    text->setProperty("format", juce::var(format));

    auto root = new juce::DynamicObject();
    root->setProperty("model", request.model);
    root->setProperty("instructions", buildSystemPrompt(request.freshPatch));
    root->setProperty("input", buildUserPrompt(request));
    root->setProperty("text", juce::var(text));
    root->setProperty("max_output_tokens", request.freshPatch ? 4096 : 2048);
    return juce::JSON::toString(juce::var(root), true);
}

juce::String buildPatchPlanSchema(const PatchRequest &request, bool includeLocalConstraints)
{
    juce::Array<juce::var> parameterIds;
    for (const auto &parameter : request.parameters)
        parameterIds.add(parameter.id);

    auto idProperty = new juce::DynamicObject();
    idProperty->setProperty("type", "integer");
    idProperty->setProperty("enum", juce::var(parameterIds));

    auto valueProperty = new juce::DynamicObject();
    valueProperty->setProperty("type", "number");
    if (includeLocalConstraints)
    {
        valueProperty->setProperty("minimum", 0.0);
        valueProperty->setProperty("maximum", 1.0);
    }

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
    auto summary = new juce::DynamicObject();
    summary->setProperty("type", "string");
    if (includeLocalConstraints)
    {
        name->setProperty("maxLength", 64);
        summary->setProperty("maxLength", 180);
    }
    auto operations = new juce::DynamicObject();
    operations->setProperty("type", "array");
    operations->setProperty("items", juce::var(operation));
    operations->setProperty("minItems", 1);
    if (includeLocalConstraints)
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

juce::String buildAnthropicRequest(const PatchRequest &request)
{
    juce::Array<juce::var> messages;
    auto user = new juce::DynamicObject();
    user->setProperty("role", "user");
    user->setProperty("content", buildUserPrompt(request));
    messages.add(juce::var(user));

    juce::var schema;
    juce::JSON::parse(buildPatchPlanSchema(request, false), schema);
    auto format = new juce::DynamicObject();
    format->setProperty("type", "json_schema");
    format->setProperty("schema", schema);
    auto outputConfig = new juce::DynamicObject();
    outputConfig->setProperty("format", juce::var(format));

    auto root = new juce::DynamicObject();
    root->setProperty("model", request.model);
    root->setProperty("system", buildSystemPrompt(request.freshPatch));
    root->setProperty("messages", juce::var(messages));
    root->setProperty("max_tokens", request.freshPatch ? 4096 : 2048);
    root->setProperty("stream", false);
    root->setProperty("output_config", juce::var(outputConfig));
    return juce::JSON::toString(juce::var(root), true);
}

bool isNativeExecutable(const juce::File &file)
{
    juce::FileInputStream input(file);
    std::array<unsigned char, 4> magic{};
    if (!input.openedOk() ||
        input.read(magic.data(), static_cast<int>(magic.size())) != static_cast<int>(magic.size()))
        return false;
    auto matches = [&magic](std::array<unsigned char, 4> expected) { return magic == expected; };
    return matches({0x7f, 'E', 'L', 'F'}) || (magic[0] == 'M' && magic[1] == 'Z') ||
           matches({0xcf, 0xfa, 0xed, 0xfe}) || matches({0xfe, 0xed, 0xfa, 0xcf}) ||
           matches({0xca, 0xfe, 0xba, 0xbe}) || matches({0xbe, 0xba, 0xfe, 0xca});
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

    auto found = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.existsAsFile() && isNativeExecutable(candidate);
    });
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

bool isSupportedTextModel(Provider provider, const juce::String &model)
{
    auto id = model.toLowerCase();
    auto excluded = id.contains("embedding") || id.contains("moderation") ||
                    id.contains("transcri") || id.contains("whisper") || id.contains("tts") ||
                    id.contains("audio") || id.contains("realtime") || id.contains("image");
    if (excluded)
        return false;

    switch (provider)
    {
    case Provider::DeepSeek:
        return id.startsWith("deepseek-");
    case Provider::MiniMax:
        return id.startsWith("minimax-m");
    case Provider::OpenAI:
        return !id.contains("search-preview") && !id.contains("deep-research") &&
               !id.contains("codex") && !(id.startsWith("gpt-5") && id.contains("-pro")) &&
               id != "gpt-4o-2024-05-13" &&
               (id.startsWith("gpt-5") || id.startsWith("gpt-4.1") || id.startsWith("gpt-4o") ||
                id.startsWith("o3") || id.startsWith("o4"));
    case Provider::Anthropic:
        return id.startsWith("claude-");
    case Provider::Gemini:
        return id.startsWith("gemini-");
    case Provider::OpenRouter:
        return id.containsChar('/') && !id.endsWith(":batch");
    case Provider::None:
    case Provider::ChatGPT:
        return false;
    }
    return false;
}

std::vector<juce::String> parseModelList(const juce::String &body, Provider provider,
                                         juce::String &error)
{
    juce::var root;
    auto parsed = juce::JSON::parse(body, root);
    if (parsed.failed())
    {
        error = "The provider returned invalid model-list JSON: " + parsed.getErrorMessage();
        return {};
    }

    auto data = root.getProperty("data", {});
    if (data.getArray() == nullptr)
        data = root.getProperty("models", {});
    auto array = data.getArray();
    if (array == nullptr)
    {
        error = "The provider response did not contain a model list.";
        return {};
    }

    std::set<juce::String> unique;
    std::vector<juce::String> models;
    for (const auto &entry : *array)
    {
        auto object = entry.getDynamicObject();
        auto id = object != nullptr ? object->getProperty("id").toString().trim()
                                    : entry.toString().trim();
        if (id.isEmpty() && object != nullptr)
            id = object->getProperty("name").toString().trim();
        if (provider == Provider::Gemini && id.startsWithIgnoreCase("models/"))
            id = id.substring(7);

        if (provider == Provider::Gemini && object != nullptr)
        {
            if (auto methods = object->getProperty("supportedGenerationMethods").getArray())
            {
                auto supportsGeneration =
                    std::any_of(methods->begin(), methods->end(), [](const auto &method) {
                        return method.toString().equalsIgnoreCase("generateContent");
                    });
                if (!supportsGeneration)
                    continue;
            }
        }

        if (provider == Provider::Anthropic && object != nullptr)
        {
            if (auto capabilities = object->getProperty("capabilities").getDynamicObject())
            {
                if (auto structured =
                        capabilities->getProperty("structured_outputs").getDynamicObject())
                    if (!static_cast<bool>(structured->getProperty("supported")))
                        continue;
            }
        }

        if (provider == Provider::OpenRouter && object != nullptr)
        {
            auto parameters = object->getProperty("supported_parameters").getArray();
            if (parameters == nullptr)
                continue;
            auto supportsStructuredOutput =
                std::any_of(parameters->begin(), parameters->end(), [](const auto &parameter) {
                    return parameter.toString() == "structured_outputs";
                });
            auto supportsTokenLimit =
                std::any_of(parameters->begin(), parameters->end(), [](const auto &parameter) {
                    return parameter.toString() == "max_tokens";
                });
            if (!supportsStructuredOutput || !supportsTokenLimit)
                continue;
        }

        if (id.isNotEmpty() && id.length() <= 160 && isSupportedTextModel(provider, id) &&
            unique.insert(id).second)
            models.push_back(id);
        if (models.size() >= (provider == Provider::OpenRouter ? 1000U : 300U))
            break;
    }

    if (models.empty())
    {
        error = "The connected account did not return any available models.";
        return {};
    }

    auto preferred = providerDefaultModel(provider);
    auto foundPreferred = std::find(models.begin(), models.end(), preferred);
    if (foundPreferred != models.end() && foundPreferred != models.begin())
        std::rotate(models.begin(), foundPreferred, foundPreferred + 1);

    auto maximumModels = provider == Provider::OpenRouter ? 1000U : 100U;
    if (models.size() > maximumModels)
        models.resize(maximumModels);
    return models;
}

} // namespace

std::vector<Provider> availableProviders()
{
    std::vector<Provider> providers;
    providers.reserve(providerConfigs.size() - 1);
    for (const auto &config : providerConfigs)
        if (config.provider != Provider::None)
            providers.push_back(config.provider);
    return providers;
}

juce::String providerId(Provider provider) { return configFor(provider).id; }

juce::String providerDisplayName(Provider provider) { return configFor(provider).displayName; }

juce::String providerDefaultModel(Provider provider) { return configFor(provider).defaultModel; }

juce::String providerKeyPage(Provider provider) { return configFor(provider).keyPage; }

Provider providerFromId(const juce::String &id)
{
    auto found = std::find_if(providerConfigs.begin(), providerConfigs.end(),
                              [&](const auto &config) { return id.equalsIgnoreCase(config.id); });
    return found != providerConfigs.end() ? found->provider : Provider::None;
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
    auto finishReason =
        choice != nullptr ? choice->getProperty("finish_reason").toString() : juce::String{};
    if (finishReason.equalsIgnoreCase("length"))
    {
        result.error =
            "The provider reached its response token limit before completing the patch plan.";
        return result;
    }
    if (finishReason.isNotEmpty() && !finishReason.equalsIgnoreCase("stop"))
    {
        result.error = "The provider did not complete the patch plan (" +
                       sanitizedText(finishReason, 80) + ").";
        return result;
    }
    auto message = choice != nullptr ? choice->getProperty("message").getDynamicObject() : nullptr;
    auto refusal = message != nullptr ? message->getProperty("refusal").toString() : juce::String{};
    if (refusal.isNotEmpty())
    {
        result.error = "The provider declined to create this patch.";
        return result;
    }
    auto content = message != nullptr ? message->getProperty("content").toString() : juce::String{};
    if (content.isEmpty())
    {
        result.error = "The provider returned an empty model answer.";
        return result;
    }
    return parsePatchPlanJson(content, freshPatch, allowedParameterIds);
}

Result parseOpenAIResponse(const juce::String &response, bool freshPatch,
                           const std::set<int> &allowedParameterIds)
{
    Result result;
    result.kind = ResultKind::Generation;

    juce::var root;
    auto parsed = juce::JSON::parse(response, root);
    if (parsed.failed())
    {
        result.error = "OpenAI returned invalid JSON: " + parsed.getErrorMessage();
        return result;
    }

    auto status = root.getProperty("status", {}).toString();
    if (status.isNotEmpty() && status != "completed")
    {
        auto reason =
            root.getProperty("incomplete_details", {}).getProperty("reason", {}).toString();
        result.error =
            status == "incomplete" && reason == "max_output_tokens"
                ? "OpenAI reached the response token limit before completing the patch plan."
                : "OpenAI did not complete the patch plan.";
        return result;
    }

    juce::String content;
    bool refused = false;
    if (auto output = root.getProperty("output", {}).getArray())
    {
        for (const auto &outputValue : *output)
        {
            auto message = outputValue.getDynamicObject();
            if (message == nullptr || message->getProperty("type").toString() != "message")
                continue;
            if (auto blocks = message->getProperty("content").getArray())
            {
                for (const auto &blockValue : *blocks)
                {
                    auto block = blockValue.getDynamicObject();
                    if (block == nullptr)
                        continue;
                    auto type = block->getProperty("type").toString();
                    if (type == "output_text")
                        content += block->getProperty("text").toString();
                    else if (type == "refusal")
                        refused = true;
                }
            }
        }
    }
    if (refused)
    {
        result.error = "OpenAI declined to create this patch.";
        return result;
    }
    if (content.isEmpty())
    {
        result.error = "OpenAI returned an empty model answer.";
        return result;
    }
    return parsePatchPlanJson(content, freshPatch, allowedParameterIds);
}

Result parseAnthropicMessage(const juce::String &response, bool freshPatch,
                             const std::set<int> &allowedParameterIds)
{
    Result result;
    result.kind = ResultKind::Generation;

    juce::var root;
    auto parsed = juce::JSON::parse(response, root);
    if (parsed.failed())
    {
        result.error = "Anthropic returned invalid JSON: " + parsed.getErrorMessage();
        return result;
    }

    auto stopReason = root.getProperty("stop_reason", {}).toString();
    if (stopReason == "max_tokens")
    {
        result.error =
            "Anthropic reached the response token limit before completing the patch plan.";
        return result;
    }
    if (stopReason == "refusal")
    {
        result.error = "Anthropic declined to create this patch.";
        return result;
    }
    if (stopReason.isNotEmpty() && stopReason != "end_turn" && stopReason != "stop_sequence")
    {
        result.error =
            "Anthropic did not complete the patch plan (" + sanitizedText(stopReason, 80) + ").";
        return result;
    }

    auto blocks = root.getProperty("content", {}).getArray();
    if (blocks == nullptr)
    {
        result.error = "Anthropic's response did not contain a model answer.";
        return result;
    }

    juce::String content;
    for (const auto &blockValue : *blocks)
    {
        auto block = blockValue.getDynamicObject();
        if (block != nullptr && block->getProperty("type").toString() == "text")
            content += block->getProperty("text").toString();
    }
    if (content.isEmpty())
    {
        result.error = "Anthropic returned an empty model answer.";
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
            if (activeJob || !jobs.empty())
                cancellationEpoch.fetch_add(1);
            cancelActiveRequest();
            cancelActiveProcess();
            while (!jobs.empty())
            {
                auto result = failure(jobs.front()->kind, "The assistant request was cancelled.");
                jobs.front()->promise.set_value(std::move(result));
                jobs.pop_front();
            }
        }
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

    bool cancel()
    {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> guard(queueMutex);
            cancelled = activeJob || !jobs.empty();
            if (cancelled)
            {
                cancellationEpoch.fetch_add(1);
                cancelActiveRequest();
                cancelActiveProcess();
            }
            while (!jobs.empty())
            {
                auto result = failure(jobs.front()->kind, "The assistant request was cancelled.");
                jobs.front()->promise.set_value(std::move(result));
                jobs.pop_front();
            }
        }
        return cancelled;
    }

  private:
    enum class JobType
    {
        Connect,
        Disconnect,
        Generate,
    };

    struct CredentialTransactionGuard
    {
        ~CredentialTransactionGuard() { release(); }

        void release()
        {
            if (interProcess != nullptr)
            {
                interProcess->exit();
                interProcess = nullptr;
            }
            if (process.owns_lock())
                process.unlock();
        }

        std::unique_lock<std::mutex> process;
        juce::InterProcessLock *interProcess{nullptr};
    };

    struct Job
    {
        JobType type{JobType::Generate};
        ResultKind kind{ResultKind::Generation};
        Provider provider{Provider::None};
        juce::String key;
        PatchRequest request;
        std::uint64_t generation{0};
        bool codexWasLoggedIn{false};
        bool codexLoginAttempted{false};
        bool codexLogoutAttempted{false};
        bool irreversibleMutationCommitted{false};
        CredentialTransactionGuard credentialTransaction;
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
        bool cancelled{false};
        bool timedOut{false};
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
            job->generation = cancellationEpoch.load();
            jobs.push_back(std::move(job));
        }
        queueCondition.notify_one();
        return future;
    }

    bool isCancelled(const Job &job) const { return job.generation != cancellationEpoch.load(); }

    bool acquireCredentialTransaction(Provider provider, std::uint64_t generation,
                                      CredentialTransactionGuard &guard, juce::String &error)
    {
        constexpr double maximumWaitMilliseconds = 30000.0;
        auto started = juce::Time::getMillisecondCounterHiRes();
        guard.process =
            std::unique_lock<std::mutex>(credentialProcessMutex(provider), std::defer_lock);
        while (!guard.process.try_lock())
        {
            if (generation != cancellationEpoch.load())
            {
                error = "The assistant request was cancelled.";
                return false;
            }
            if (juce::Time::getMillisecondCounterHiRes() - started >= maximumWaitMilliseconds)
            {
                error = "Another Surge XT instance is updating this assistant connection.";
                return false;
            }
            juce::Thread::sleep(20);
        }

        auto &interProcess = credentialTransactionLock(provider);
        while (!interProcess.enter(100))
        {
            if (generation != cancellationEpoch.load())
            {
                error = "The assistant request was cancelled.";
                return false;
            }
            if (juce::Time::getMillisecondCounterHiRes() - started >= maximumWaitMilliseconds)
            {
                error = "Another Surge XT process is updating this assistant connection.";
                return false;
            }
        }
        guard.interProcess = &interProcess;
        return true;
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

    static bool codexStatusSaysLoggedOut(const ProcessResponse &status)
    {
        return status.output.containsIgnoreCase("not logged in") ||
               status.output.containsIgnoreCase("not authenticated") ||
               status.output.containsIgnoreCase("no login");
    }

    juce::String settleCodexCancellation(Job &job)
    {
        if (job.provider == Provider::ChatGPT && job.codexLoginAttempted && !job.codexWasLoggedIn)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
                return "The Codex runtime could not be found to restore the previous login state.";
            auto generation = cancellationEpoch.load();
            auto status =
                runProcess({codex.getFullPathName(), "login", "status"}, generation, 15000);
            if (status.cancelled || status.timedOut)
                return "Codex login status could not be checked while restoring the previous login "
                       "state.";
            if (status.exitCode == 0 && status.output.containsIgnoreCase("ChatGPT"))
            {
                auto logout = runProcess({codex.getFullPathName(), "logout"}, generation, 15000);
                if (logout.cancelled || logout.timedOut || logout.exitCode != 0)
                    return "Codex sign-out failed while restoring the previous login state.";
            }
            else if (status.exitCode == 0 || !codexStatusSaysLoggedOut(status))
            {
                return "Codex returned an unknown login state while restoring authentication.";
            }
            job.codexLoginAttempted = false;
            return {};
        }
        if (job.provider == Provider::ChatGPT && job.codexLogoutAttempted)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
                return "The Codex runtime could not be found to verify sign-out.";
            auto generation = cancellationEpoch.load();
            auto logout = runProcess({codex.getFullPathName(), "logout"}, generation, 15000);
            if (!logout.cancelled && !logout.timedOut && logout.exitCode == 0)
            {
                job.irreversibleMutationCommitted = true;
                return {};
            }
            if (logout.cancelled || logout.timedOut)
                return "Codex sign-out could not be completed after cancellation.";

            auto status =
                runProcess({codex.getFullPathName(), "login", "status"}, generation, 15000);
            if (status.cancelled || status.timedOut)
                return "Codex login status could not be checked after sign-out.";
            if (codexStatusSaysLoggedOut(status))
            {
                job.irreversibleMutationCommitted = true;
                return {};
            }
            if (status.exitCode == 0 && status.output.containsIgnoreCase("ChatGPT"))
                return {};
            return "Codex returned an unknown login state after sign-out.";
        }
        return {};
    }

    ProcessResponse runProcess(const juce::StringArray &arguments, std::uint64_t generation,
                               int timeoutMilliseconds = 300000)
    {
        ProcessResponse response;
        if (generation != cancellationEpoch.load())
        {
            response.cancelled = true;
            response.error = "The assistant request was cancelled.";
            response.output = response.error;
            return response;
        }

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
        if (generation != cancellationEpoch.load())
        {
            response.cancelled = true;
            process->kill();
        }

        juce::MemoryOutputStream output;
        std::thread reader([&process, &output]() {
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
                    output.write(buffer.data(),
                                 static_cast<size_t>(std::min(bytesRead, remaining)));
                }
            }
        });

        auto started = juce::Time::getMillisecondCounterHiRes();
        while (!response.cancelled)
        {
            if (generation != cancellationEpoch.load())
            {
                response.cancelled = true;
                process->kill();
                break;
            }
            if (!process->isRunning())
                break;
            if (juce::Time::getMillisecondCounterHiRes() - started >= timeoutMilliseconds)
            {
                response.timedOut = true;
                process->kill();
                break;
            }
            juce::Thread::sleep(20);
        }

        reader.join();
        process->waitForProcessToFinish(1000);
        response.exitCode = process->getExitCode();
        response.output = output.toString();
        if (response.cancelled)
            response.error = "The assistant request was cancelled.";
        else if (response.timedOut)
            response.error = "The Codex runtime did not finish in time.";

        {
            std::lock_guard<std::mutex> guard(processMutex);
            activeProcess = nullptr;
        }
        return response;
    }

    static juce::String requestHeaders(Provider provider, const juce::String &url,
                                       const juce::String &key)
    {
        juce::String headers = "Accept: application/json\r\nContent-Type: application/json\r\n"
                               "User-Agent: Surge-XT-Assistant\r\n";
        if (provider == Provider::Anthropic)
            headers += "x-api-key: " + key + "\r\nanthropic-version: 2023-06-01\r\n";
        else if (provider == Provider::Gemini && !url.contains("/openai"))
            headers += "x-goog-api-key: " + key + "\r\n";
        else
            headers += "Authorization: Bearer " + key + "\r\n";

        if (provider == Provider::OpenRouter)
            headers += "HTTP-Referer: https://surge-synth-team.org/\r\n"
                       "X-OpenRouter-Title: Surge XT\r\n";
        return headers;
    }

    HttpResponse performHttp(Provider provider, const juce::String &url, const juce::String &method,
                             const juce::String &key, const juce::String &body,
                             std::uint64_t generation)
    {
        HttpResponse response;
        if (generation != cancellationEpoch.load())
        {
            response.error = "The assistant request was cancelled.";
            return response;
        }

        auto requestUrl = juce::URL(url);
        if (method == "POST")
            requestUrl = requestUrl.withPOSTData(body);

        auto stream = std::make_unique<juce::WebInputStream>(requestUrl, method == "POST");
        stream->withCustomRequestCommand(method)
            .withConnectionTimeout(method == "POST" ? 120000 : 30000)
            .withNumRedirectsToFollow(0)
            .withExtraHeaders(requestHeaders(provider, url, key));

        {
            std::lock_guard<std::mutex> guard(streamMutex);
            activeStream = stream.get();
        }
        if (generation != cancellationEpoch.load())
            stream->cancel();

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

    Result processConnect(Job &job)
    {
        if (isCancelled(job))
            return failure(ResultKind::Connection, "The assistant request was cancelled.");

        Result result;
        result.kind = ResultKind::Connection;

        if (job.provider == Provider::ChatGPT)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
            {
                result.error =
                    "Install the official native Codex CLI first, then try ChatGPT sign-in again.";
                return result;
            }

            juce::String lockError;
            if (!acquireCredentialTransaction(job.provider, job.generation,
                                              job.credentialTransaction, lockError))
            {
                result.error = lockError;
                return result;
            }
            if (isCancelled(job))
                return failure(ResultKind::Connection, "The assistant request was cancelled.");

            auto status =
                runProcess({codex.getFullPathName(), "login", "status"}, job.generation, 15000);
            if (status.cancelled || status.timedOut)
            {
                result.error = status.error;
                return result;
            }
            if (status.exitCode == 0 && !status.output.containsIgnoreCase("ChatGPT"))
            {
                result.error =
                    "Codex already has non-ChatGPT authentication. Sign out of Codex explicitly "
                    "before connecting ChatGPT Plus.";
                return result;
            }
            if (status.exitCode != 0 && !codexStatusSaysLoggedOut(status))
            {
                result.error = "Codex login status could not be determined: " +
                               sanitizedText(status.output, 300);
                return result;
            }
            job.codexWasLoggedIn =
                status.exitCode == 0 && status.output.containsIgnoreCase("ChatGPT");
            if (!job.codexWasLoggedIn)
            {
                job.codexLoginAttempted = true;
                auto login = runProcess({codex.getFullPathName(), "login"}, job.generation, 600000);
                if (login.cancelled || login.timedOut || login.exitCode != 0)
                {
                    result.error = login.error.isNotEmpty() ? login.error
                                                            : "ChatGPT sign-in failed: " +
                                                                  sanitizedText(login.output, 300);
                    return result;
                }
                status =
                    runProcess({codex.getFullPathName(), "login", "status"}, job.generation, 15000);
            }

            if (status.cancelled || status.timedOut || status.exitCode != 0 ||
                !status.output.containsIgnoreCase("ChatGPT"))
            {
                result.error = "Codex did not report an active ChatGPT subscription login.";
                return result;
            }
            result.models = {providerDefaultModel(Provider::ChatGPT)};
            result.credentialPersistent = true;
            result.ok = true;
            job.irreversibleMutationCommitted = job.codexLoginAttempted;
            return result;
        }

        auto key = job.key.trim();
        if (job.provider == Provider::None || key.isEmpty() || key.containsAnyOf("\r\n"))
        {
            result.error = "Enter a valid API key.";
            return result;
        }

        if (job.provider == Provider::OpenRouter)
        {
            auto validation = performHttp(job.provider, providerBaseUrl(job.provider) + "/key",
                                          "GET", key, {}, job.generation);
            if (validation.error.isNotEmpty())
            {
                result.error = validation.error;
                return result;
            }
            if (validation.statusCode < 200 || validation.statusCode >= 300)
            {
                result.error = "OpenRouter rejected the connection (HTTP " +
                               juce::String(validation.statusCode) +
                               "): " + providerErrorFromBody(validation.body);
                return result;
            }
        }

        auto modelsUrl = providerBaseUrl(job.provider) + "/models";
        if (job.provider == Provider::Anthropic)
            modelsUrl = providerBaseUrl(job.provider) + "/v1/models?limit=100";
        else if (job.provider == Provider::Gemini)
            modelsUrl = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=100";
        else if (job.provider == Provider::OpenRouter)
            modelsUrl = providerBaseUrl(job.provider) + "/models/user?limit=1000";

        auto response = performHttp(job.provider, modelsUrl, "GET", key, {}, job.generation);
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
        result.models = parseModelList(response.body, job.provider, modelError);
        if (result.models.empty())
        {
            result.error = modelError.isNotEmpty()
                               ? modelError
                               : "The connected account did not return a supported text model.";
            return result;
        }

        if (isCancelled(job))
            return failure(ResultKind::Connection, "The assistant request was cancelled.");

        juce::String lockError;
        if (!acquireCredentialTransaction(job.provider, job.generation, job.credentialTransaction,
                                          lockError))
        {
            result.error = lockError;
            return result;
        }
        if (isCancelled(job))
            return failure(ResultKind::Connection, "The assistant request was cancelled.");

        auto stored = writeCredential(job.provider, key);
        if (!stored.ok)
        {
            result.error = stored.error;
            return result;
        }
        job.irreversibleMutationCommitted = true;
        result.credentialPersistent = stored.persistent;
        result.ok = true;
        return result;
    }

    Result processDisconnect(Job &job)
    {
        if (isCancelled(job))
            return failure(ResultKind::Connection, "The assistant request was cancelled.");

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
            juce::String lockError;
            if (!acquireCredentialTransaction(job.provider, job.generation,
                                              job.credentialTransaction, lockError))
            {
                result.error = lockError;
                return result;
            }
            if (isCancelled(job))
                return failure(ResultKind::Connection, "The assistant request was cancelled.");
            job.codexLogoutAttempted = true;
            auto logout = runProcess({codex.getFullPathName(), "logout"}, job.generation, 15000);
            result.ok = !logout.cancelled && !logout.timedOut && logout.exitCode == 0;
            if (!result.ok)
                result.error = logout.error.isNotEmpty() ? logout.error
                                                         : "ChatGPT sign-out failed: " +
                                                               sanitizedText(logout.output, 300);
            job.irreversibleMutationCommitted = result.ok;
            result.credentialPersistent = true;
            return result;
        }

        juce::String lockError;
        if (!acquireCredentialTransaction(job.provider, job.generation, job.credentialTransaction,
                                          lockError))
        {
            result.error = lockError;
            return result;
        }
        if (isCancelled(job))
            return failure(ResultKind::Connection, "The assistant request was cancelled.");

        auto erased = eraseCredential(job.provider);
        result.ok = erased.ok;
        result.credentialPersistent = erased.persistent;
        result.error = erased.error;
        job.irreversibleMutationCommitted = erased.ok;
        return result;
    }

    Result processGenerate(const Job &job)
    {
        if (isCancelled(job))
            return failure(ResultKind::Generation, "The assistant request was cancelled.");
        if (job.request.provider == Provider::None || job.request.model.isEmpty())
            return failure(ResultKind::Generation,
                           "Select and configure an assistant connection first.");

        if (job.request.provider == Provider::ChatGPT)
        {
            auto codex = findCodexExecutable();
            if (!codex.existsAsFile())
                return failure(ResultKind::Generation,
                               "The official native Codex CLI is required for ChatGPT Plus.");

            CredentialTransactionGuard credentialGuard;
            juce::String lockError;
            if (!acquireCredentialTransaction(job.request.provider, job.generation, credentialGuard,
                                              lockError))
                return failure(ResultKind::Generation, lockError);

            auto temporaryRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("surge-xt-assistant-" + juce::Uuid().toString());
            if (temporaryRoot.createDirectory().failed())
                return failure(ResultKind::Generation,
                               "Could not create a private temporary assistant directory.");
            ScopedDirectoryRemoval cleanup(temporaryRoot);
#if !JUCE_WINDOWS
            if (::chmod(temporaryRoot.getFullPathName().toRawUTF8(), S_IRWXU) != 0)
                return failure(ResultKind::Generation,
                               "Could not secure the temporary assistant directory.");
#endif

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
            auto execution = runProcess(arguments, job.generation);
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

        CredentialResult credential;
        {
            CredentialTransactionGuard guard;
            juce::String lockError;
            if (!acquireCredentialTransaction(job.request.provider, job.generation, guard,
                                              lockError))
                return failure(ResultKind::Generation, lockError);
            credential = readCredential(job.request.provider);
        }
        if (!credential.ok)
            return failure(ResultKind::Generation, credential.error);

        auto protocol = providerProtocol(job.request.provider);
        auto usesAnthropic = protocol == ProviderProtocol::Anthropic;
        auto usesOpenAIResponses = protocol == ProviderProtocol::OpenAIResponses;
        auto requestBody = usesAnthropic         ? buildAnthropicRequest(job.request)
                           : usesOpenAIResponses ? buildOpenAIResponseRequest(job.request)
                                                 : buildChatRequest(job.request);
        auto endpoint =
            providerBaseUrl(job.request.provider) + (usesAnthropic         ? "/v1/messages"
                                                     : usesOpenAIResponses ? "/responses"
                                                                           : "/chat/completions");
        auto response = performHttp(job.request.provider, endpoint, "POST", credential.secret,
                                    requestBody, job.generation);
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
        if (usesAnthropic)
            return parseAnthropicMessage(response.body, job.request.freshPatch, allowedIds);
        if (usesOpenAIResponses)
            return parseOpenAIResponse(response.body, job.request.freshPatch, allowedIds);
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
                activeJob = true;
            }

            auto action = job->type == JobType::Connect
                              ? juce::String("connect")
                              : (job->type == JobType::Disconnect ? juce::String("disconnect")
                                                                  : juce::String("generate"));
            auto started = juce::Time::getMillisecondCounterHiRes();
            writeDiagnostic(action, job->provider, "started");
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
            auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
            bool succeeded = false;
            juce::String error;
            auto publish = [&]() {
                succeeded = result.ok;
                error = result.error;
                job->key.clear();
                job->credentialTransaction.release();
                job->promise.set_value(std::move(result));
                activeJob = false;
            };
            bool wasCancelled = false;
            bool requiresSettlement = false;
            {
                std::lock_guard<std::mutex> guard(queueMutex);
                wasCancelled = isCancelled(*job);
                auto failedCodexMutation = job->provider == Provider::ChatGPT && !result.ok &&
                                           (job->codexLoginAttempted || job->codexLogoutAttempted);
                requiresSettlement =
                    (wasCancelled && !job->irreversibleMutationCommitted) || failedCodexMutation;
                if (!requiresSettlement)
                    publish();
            }
            if (requiresSettlement)
            {
                auto settlementError = settleCodexCancellation(*job);
                if (job->irreversibleMutationCommitted)
                {
                    result = {};
                    result.kind = job->kind;
                    result.ok = true;
                    result.credentialPersistent = true;
                }
                else if (wasCancelled)
                {
                    result = failure(job->kind, "The assistant request was cancelled.");
                    if (settlementError.isNotEmpty())
                        result.error +=
                            " The previous authentication state could not be restored: " +
                            settlementError;
                }
                else if (settlementError.isNotEmpty())
                    result.error += " Authentication cleanup also failed: " + settlementError;
                std::lock_guard<std::mutex> guard(queueMutex);
                publish();
            }
            writeDiagnostic(action, job->provider, succeeded ? "succeeded" : "failed", elapsed,
                            error);
        }
    }

    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<std::unique_ptr<Job>> jobs;
    bool stopping{false};
    bool activeJob{false};
    std::atomic<std::uint64_t> cancellationEpoch{0};
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

bool Client::cancel() { return impl->cancel(); }

} // namespace Assistant
} // namespace Surge
