"""MCP tool definitions -> OpenAI function tools."""

import copy

# Keywords some OpenAI-compatible servers reject when they turn the schema into a grammar.
_UNSUPPORTED_KEYWORDS = frozenset({
    "$schema", "$id", "$ref", "$defs", "definitions", "additionalProperties",
    "minItems", "maxItems", "uniqueItems", "minimum", "maximum", "exclusiveMinimum",
    "exclusiveMaximum", "multipleOf", "minLength", "maxLength", "pattern", "format",
    "default", "examples", "title", "const", "not", "allOf", "if", "then", "else",
})

_ARTICLES = {"integer": "an integer", "array": "an array", "object": "an object"}


def function_tool(name, tool, simplify=False):
    """`tool` is an MCP Tool (name, description, inputSchema); `name` is what the model will call it."""
    schema = tool.get("inputSchema") or {}
    parameters = simplify_schema(schema) if simplify else copy.deepcopy(schema)
    # Servers differ on whether a function without parameters may omit them.
    parameters.setdefault("type", "object")
    parameters.setdefault("properties", {})
    return {"type": "function",
            "function": {"name": name, "description": tool.get("description") or tool.get("title") or "",
                         "parameters": parameters}}


def simplify_schema(schema):
    """A permissive copy of a JSON Schema for picky servers.

    `oneOf`/`anyOf` and array size limits are moved into `description`; _UNSUPPORTED_KEYWORDS are removed.
    """
    if not isinstance(schema, dict):
        return schema
    simple = {}
    notes = []
    for key, value in schema.items():
        if key in ("oneOf", "anyOf") or key in _UNSUPPORTED_KEYWORDS:
            continue
        if key == "properties":
            simple[key] = {name: simplify_schema(sub) for name, sub in value.items()}
        elif key == "items":
            simple[key] = simplify_schema(value)
        else:
            simple[key] = copy.deepcopy(value)

    alternatives = schema.get("oneOf") or schema.get("anyOf")
    if alternatives:
        simple.pop("type", None)
        notes.append("Accepts " + " or ".join(_describe(alt) for alt in alternatives) + ".")
    elif schema.get("type") == "array":
        size = _size_phrase(schema, schema.get("items", {}).get("type", "value"))
        if size:
            notes.append(size[0].upper() + size[1:] + ".")

    if notes:
        simple["description"] = " ".join(filter(None, [schema.get("description", ""), *notes]))
    return simple


def _describe(schema):
    if "enum" in schema:
        return "one of " + ", ".join(_literal(v) for v in schema["enum"])
    kind = schema.get("type", "any value")
    if kind == "array":
        item = schema.get("items", {}).get("type", "value")
        return f"an array of {_size_phrase(schema, item) or item + 's'}"
    return _ARTICLES.get(kind, f"a {kind}")


def _size_phrase(schema, item="value"):
    low, high = schema.get("minItems"), schema.get("maxItems")
    if low is None and high is None:
        return ""
    if low == high:
        return f"exactly {low} {item}s"
    if low is None:
        return f"at most {high} {item}s"
    if high is None:
        return f"at least {low} {item}s"
    return f"{low} to {high} {item}s"


def _literal(value):
    return f'"{value}"' if isinstance(value, str) else str(value)
