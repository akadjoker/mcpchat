#include "agent/Agent.h"

#include "agent/Tools.h"
#include "llm/Wire.h"
#include "util/Base64.h"
#include "util/Cancel.h"
#include "util/Strings.h"

namespace mcpchat
{

const char* const kDefaultSystemPrompt =
    "You are an assistant that works through the tools of the connected MCP servers. Use them to do what the user "
    "asks,\ncheck the results, and answer briefly in the user's language. If a tool returns an error, read it, fix the "
    "arguments\nand try again instead of repeating the same call. Tool arguments must be a single JSON object.\n";

Agent::Agent(LlmProvider& provider, ToolHost& host, AgentListener& listener, AgentConfig config, Confirm confirm)
    : mProvider(provider), mHost(host), mListener(listener), mConfig(std::move(config)), mConfirm(std::move(confirm))
{
}

void Agent::reset()
{
    mMessages.clear();
}

std::string Agent::systemPrompt() const
{
    std::string prompt = trim(mConfig.systemPrompt.empty() ? std::string(kDefaultSystemPrompt) : mConfig.systemPrompt);
    for (const auto& part : mHost.instructions())
        prompt += "\n\n# " + part.first + "\n" + part.second;
    return prompt + "\n";
}

Json Agent::toolDefinitions() const
{
    Json tools = Json::array();
    for (const auto& entry : mHost.tools())
        tools.push_back(functionTool(entry.first, entry.second.tool, mConfig.simplifySchema));
    return tools;
}

RunResult Agent::run(const Json& userContent, CancelToken* cancel)
{
    const Json tools = toolDefinitions();
    mMessages.push_back({{"role", "user"}, {"content", userContent}});
    const Json system = {{"role", "system"}, {"content", systemPrompt()}};

    for (int step = 1; step <= mConfig.maxSteps; ++step)
    {
        if (cancel && cancel->isSet())
            return {RunReason::Cancelled, step - 1};
        mListener.onStep(step, mConfig.maxSteps);
        pruneHistory(mMessages, mConfig.prune);

        std::vector<Json> request;
        request.reserve(mMessages.size() + 1);
        request.push_back(system);
        request.insert(request.end(), mMessages.begin(), mMessages.end());
        AssistantMessage reply;
        try
        {
            reply = mProvider.complete(request, tools, [this](const std::string& text) { mListener.onTextDelta(text); },
                                       cancel);
        }
        catch (const Cancelled&)
        {
            return {RunReason::Cancelled, step};
        }
        catch (const LlmError& error)
        {
            mListener.onError(error.what());
            return {RunReason::Error, step};
        }

        mMessages.push_back(assistantToHistory(reply));
        if (reply.toolCalls.empty())
        {
            if (trim(reply.content).empty())
                mListener.onError("The model returned an empty answer.");
            return {RunReason::Done, step};
        }
        RunReason stop = RunReason::Done;
        if (!runToolCalls(reply.toolCalls, cancel, stop))
            return {stop, step};
    }
    mListener.onError("Stopped after " + std::to_string(mConfig.maxSteps) +
                      " steps without a final answer. Send another message to let the model continue.");
    return {RunReason::MaxSteps, mConfig.maxSteps};
}

bool Agent::runToolCalls(const std::vector<ToolCall>& calls, CancelToken* cancel, RunReason& stop)
{
    for (std::size_t position = 0; position < calls.size(); ++position)
    {
        if (cancel && cancel->isSet())
        {
            // Every tool_call needs an answer or the next request would be invalid.
            for (std::size_t skipped = position; skipped < calls.size(); ++skipped)
                addToolMessage(calls[skipped].id, Outcome{"Cancelled by the user before it ran."});
            stop = RunReason::Cancelled;
            return false;
        }
        const Outcome outcome = execute(calls[position], cancel);
        addToolMessage(calls[position].id, outcome);
        if (outcome.fatal)
        {
            for (std::size_t skipped = position + 1; skipped < calls.size(); ++skipped)
                addToolMessage(calls[skipped].id, Outcome{"Not run: the server is unreachable."});
            stop = cancel && cancel->isSet() ? RunReason::Cancelled : RunReason::Error;
            return false;
        }
    }
    return true;
}

Agent::Outcome Agent::execute(const ToolCall& call, CancelToken* cancel)
{
    mListener.onToolCall(call.id, call.name, call.arguments ? *call.arguments : Json(call.rawArguments));
    const Outcome outcome = attempt(call, cancel);
    mListener.onToolResult(call.id, call.name, outcome.text, outcome.isError, outcome.images);
    if (outcome.fatal && !(cancel && cancel->isSet()))
        mListener.onError(outcome.text);
    return outcome;
}

bool Agent::needsConfirmation(const ExposedTool& tool) const
{
    if (mConfig.confirm == ConfirmPolicy::Never || !mConfirm)
        return false;
    if (mConfig.confirm == ConfirmPolicy::Writes)
        return !tool.readOnly();
    return tool.destructive();
}

Agent::Outcome Agent::attempt(const ToolCall& call, CancelToken* cancel)
{
    if (!call.error.empty())
        return {"ERROR: " + call.error + ". Send the arguments as one JSON object.", true};
    const auto found = mHost.tools().find(call.name);
    if (found == mHost.tools().end())
    {
        std::string names;
        for (const auto& entry : mHost.tools())
            names += (names.empty() ? "" : ", ") + entry.first;
        return {"ERROR: there is no tool '" + call.name + "'. Available tools: " + names + ".", true};
    }
    const Json arguments = call.arguments ? *call.arguments : Json::object();
    if (needsConfirmation(found->second) && !mConfirm(call.name, arguments))
        return {"The user declined to run " + call.name + ". Do not retry it; continue without it or ask the user.",
                true};

    ToolResult result;
    try
    {
        result = mHost.call(call.name, arguments, cancel);
    }
    catch (const Cancelled&)
    {
        return Outcome("Cancelled by the user while it ran.", true, true);
    }
    catch (const TransportError& error)
    {
        return Outcome(std::string("ERROR: ") + error.what(), true, true);
    }
    catch (const McpError& error)
    {
        return {"ERROR: " + error.message, true};
    }

    std::string text = result.text();
    if (text.empty() && !result.structured.is_null())
        text = dump(result.structured);
    Outcome outcome;
    outcome.isError = result.isError;
    for (const Json& block : result.images())
    {
        ToolImage image;
        image.mimeType = block.value("mimeType", std::string("image/png"));
        if (base64Decode(block["data"].get<std::string>(), image.bytes))
            outcome.images.push_back(std::move(image));
    }
    outcome.text = truncate(text.empty() ? std::string("(no text)") : text, mConfig.maxResultChars);
    if (!outcome.images.empty())
    {
        if (mProvider.supportsImages())
            outcome.image = result.images().front();
        else
            outcome.text += "\n(The tool returned an image, but you cannot see images.)";
    }
    return outcome;
}

void Agent::addToolMessage(const std::string& id, const Outcome& outcome)
{
    Json message = {{"role", "tool"}, {"tool_call_id", id}, {"content", outcome.text}};
    if (outcome.image.is_object())
        message["image"] = {{"mimeType", outcome.image.value("mimeType", std::string("image/png"))},
                            {"data", outcome.image["data"]}};
    mMessages.push_back(message);
}

Json Agent::exportConversation() const
{
    Json messages = Json::array();
    for (Json message : mMessages)
    {
        if (message.contains("image"))
            message["image"]["data"] = "<omitted>";
        if (message.contains("content") && message["content"].is_array())
        {
            for (Json& part : message["content"])
                if (part.value("type", "") == "image_url")
                    part["image_url"]["url"] = "<omitted>";
        }
        messages.push_back(message);
    }
    return {{"system_prompt", systemPrompt()}, {"messages", messages}};
}

} // namespace mcpchat
