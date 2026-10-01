#include "agent/Tools.h"

#include <cctype>
#include <set>

namespace mcpchat
{

namespace
{
// Keywords some OpenAI-compatible servers reject when they turn the schema into a grammar.
const std::set<std::string>& unsupportedKeywords()
{
    static const std::set<std::string> keywords = {
        "$schema", "$id", "$ref", "$defs", "definitions", "additionalProperties", "minItems", "maxItems",
        "uniqueItems", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf", "minLength",
        "maxLength", "pattern", "format", "default", "examples", "title", "const", "not", "allOf", "if", "then",
        "else"};
    return keywords;
}

std::string literal(const Json& value)
{
    return value.is_string() ? "\"" + value.get<std::string>() + "\"" : dump(value);
}

std::string typeName(const Json& schema, const char* fallback)
{
    return schema.is_object() && schema.contains("type") && schema["type"].is_string() ? schema["type"].get<std::string>()
                                                                                       : std::string(fallback);
}

std::string sizePhrase(const Json& schema, const std::string& item)
{
    const bool hasLow = schema.contains("minItems") && schema["minItems"].is_number();
    const bool hasHigh = schema.contains("maxItems") && schema["maxItems"].is_number();
    if (!hasLow && !hasHigh)
        return std::string();
    const std::string low = hasLow ? dump(schema["minItems"]) : std::string();
    const std::string high = hasHigh ? dump(schema["maxItems"]) : std::string();
    if (hasLow && hasHigh && low == high)
        return "exactly " + low + " " + item + "s";
    if (!hasLow)
        return "at most " + high + " " + item + "s";
    if (!hasHigh)
        return "at least " + low + " " + item + "s";
    return low + " to " + high + " " + item + "s";
}

std::string describe(const Json& schema)
{
    if (schema.is_object() && schema.contains("enum") && schema["enum"].is_array())
    {
        std::string out = "one of ";
        bool first = true;
        for (const Json& value : schema["enum"])
        {
            out += (first ? "" : ", ") + literal(value);
            first = false;
        }
        return out;
    }
    const std::string kind = typeName(schema, "any value");
    if (kind == "array")
    {
        const std::string item = typeName(schema.value("items", Json::object()), "value");
        const std::string size = sizePhrase(schema, item);
        return "an array of " + (size.empty() ? item + "s" : size);
    }
    if (kind == "integer" || kind == "array" || kind == "object")
        return "an " + kind;
    return "a " + kind;
}
} // namespace

Json simplifySchema(const Json& schema)
{
    if (!schema.is_object())
        return schema;
    Json simple = Json::object();
    std::string notes;
    for (auto it = schema.begin(); it != schema.end(); ++it)
    {
        const std::string& key = it.key();
        if (key == "oneOf" || key == "anyOf" || unsupportedKeywords().count(key))
            continue;
        if (key == "properties" && it.value().is_object())
        {
            Json properties = Json::object();
            for (auto property = it.value().begin(); property != it.value().end(); ++property)
                properties[property.key()] = simplifySchema(property.value());
            simple[key] = properties;
        }
        else if (key == "items")
            simple[key] = simplifySchema(it.value());
        else
            simple[key] = it.value();
    }

    const Json* alternatives = nullptr;
    if (schema.contains("oneOf") && schema["oneOf"].is_array() && !schema["oneOf"].empty())
        alternatives = &schema["oneOf"];
    else if (schema.contains("anyOf") && schema["anyOf"].is_array() && !schema["anyOf"].empty())
        alternatives = &schema["anyOf"];
    if (alternatives)
    {
        simple.erase("type");
        notes = "Accepts ";
        bool first = true;
        for (const Json& alternative : *alternatives)
        {
            notes += (first ? "" : " or ") + describe(alternative);
            first = false;
        }
        notes += ".";
    }
    else if (typeName(schema, "") == "array")
    {
        std::string size = sizePhrase(schema, typeName(schema.value("items", Json::object()), "value"));
        if (!size.empty())
        {
            size[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(size[0])));
            notes = size + ".";
        }
    }
    if (!notes.empty())
    {
        const std::string original = schema.value("description", std::string());
        simple["description"] = original.empty() ? notes : original + " " + notes;
    }
    return simple;
}

Json functionTool(const std::string& name, const Json& tool, bool simplify)
{
    const Json schema = tool.contains("inputSchema") && tool["inputSchema"].is_object() ? tool["inputSchema"]
                                                                                       : Json::object();
    Json parameters = simplify ? simplifySchema(schema) : schema;
    // Servers differ on whether a function without parameters may omit them.
    if (!parameters.contains("type"))
        parameters["type"] = "object";
    if (!parameters.contains("properties"))
        parameters["properties"] = Json::object();
    std::string description = tool.value("description", std::string());
    if (description.empty())
        description = tool.value("title", std::string());
    return {{"type", "function"},
            {"function", {{"name", name}, {"description", description}, {"parameters", parameters}}}};
}

} // namespace mcpchat
