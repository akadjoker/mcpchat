
OMITTED = "[...omitted from history]"


def prune_history(messages, *, keep_images, recent_results, old_result_chars, max_chars):
    """Shrinks `messages` in place; the current turn is always kept."""
    _drop_old_images(messages, keep_images)
    _shorten_old_results(messages, recent_results, old_result_chars)
    while _size(messages) > max_chars:
        next_turn = _next_user_index(messages)
        if next_turn is None:
            break
        del messages[:next_turn]


def truncate(text, limit):
    if len(text) <= limit:
        return text
    return f"{text[:limit]}\n[truncated: {len(text) - limit} more characters]"


def _tool_messages(messages):
    return [m for m in messages if m["role"] == "tool"]


def _drop_old_images(messages, keep_images):
    with_image = [m for m in _tool_messages(messages) if m.get("image")]
    for message in with_image[:max(0, len(with_image) - keep_images)]:
        del message["image"]
        message["content"] += "\n[screenshot no longer attached]"


def _shorten_old_results(messages, recent_results, limit):
    tools = _tool_messages(messages)
    for message in tools[:max(0, len(tools) - recent_results)]:
        content = message["content"]
        if len(content) > limit and OMITTED not in content:
            message["content"] = f"{content[:limit]}\n{OMITTED}"


def _size(messages):
    total = 0
    for message in messages:
        total += len(message.get("content") or "")
        for call in message.get("tool_calls") or []:
            total += len(call["function"]["arguments"])
    return total


def _next_user_index(messages):
    users = [i for i, m in enumerate(messages) if m["role"] == "user"]
    return users[1] if len(users) > 1 else None
