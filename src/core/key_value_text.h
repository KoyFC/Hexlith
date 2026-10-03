#ifndef HEXLITH_CORE_KEY_VALUE_TEXT_H
#define HEXLITH_CORE_KEY_VALUE_TEXT_H

#include "core/result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::core {

// One `key = value` line, with the line it came from so that a complaint
// about it can name the place a person has to go and edit.
struct KeyValue {
    std::string key;
    std::string value;
    std::size_t line = 0;
};

// Parses the line-oriented text the project uses instead of a configuration
// format with a parser attached. ADR 0008 chose it for the workspace
// configuration and ADR 0006 for the analysis artefacts, so that neither
// costs a dependency.
//
// A blank line is ignored, and so is one whose first character that is not
// space is #. Whitespace around the key and around the value is removed;
// whitespace inside the value is kept, because a path may contain a space.
//
// A repeated key is an error rather than a silent last-one-wins, since the
// case it arises from is a file edited twice with the earlier line left
// above, and quietly honouring one of the two is how a tool ends up reading
// from somewhere nobody meant.
Result<std::vector<KeyValue>> ParseKeyValueText(std::string_view text);

// The entry for name, or nullptr when the text did not set it.
const KeyValue* Find(const std::vector<KeyValue>& entries, std::string_view name);

}  // namespace hexlith::core

#endif  // HEXLITH_CORE_KEY_VALUE_TEXT_H
