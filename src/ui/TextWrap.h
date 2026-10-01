#ifndef MCPCHAT_TEXT_WRAP_H
#define MCPCHAT_TEXT_WRAP_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpchat
{

// Breaks UTF-8 text into lines no wider than `width`: at spaces when it can, inside a word when the word alone is too
// wide, and always at '\n'. `measure` is called one character at a time: a line's width is the sum of its characters'.
std::vector<std::string> wrapText(std::string_view text, float width,
                                  const std::function<float(std::string_view)>& measure);

} // namespace mcpchat

#endif
