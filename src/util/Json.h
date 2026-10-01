#ifndef MCPCHAT_JSON_H
#define MCPCHAT_JSON_H

#include <nlohmann/json.hpp>

namespace mcpchat
{
using Json = nlohmann::json;

// Compact text, as sent on the wire.
inline std::string dump(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}
} // namespace mcpchat

#endif
