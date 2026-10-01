#ifndef MCPCHAT_TOOLS_H
#define MCPCHAT_TOOLS_H

#include "util/Json.h"

namespace mcpchat
{

// An MCP Tool (name, description, inputSchema) as an OpenAI function tool called `name`.
Json functionTool(const std::string& name, const Json& tool, bool simplify);

// A permissive copy of a JSON Schema for picky servers: oneOf/anyOf and array size limits move into `description`,
// keywords such servers reject are dropped.
Json simplifySchema(const Json& schema);

} // namespace mcpchat

#endif
