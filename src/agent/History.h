#ifndef MCPCHAT_HISTORY_H
#define MCPCHAT_HISTORY_H

#include "util/Json.h"

#include <vector>

namespace mcpchat
{

struct PruneLimits
{
    std::size_t keepImages = 2;
    std::size_t recentResults = 6;
    std::size_t oldResultChars = 600;
    std::size_t maxChars = 120000;
};

// Shrinks the conversation in place: old screenshots go first, then the text of old tool results, then whole old
// turns. The current turn is always kept.
void pruneHistory(std::vector<Json>& messages, const PruneLimits& limits);

} // namespace mcpchat

#endif
