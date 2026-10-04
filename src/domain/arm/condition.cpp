#include "domain/arm/condition.h"

#include <array>

namespace hexlith::domain::arm {

std::string_view ConditionSuffix(Condition condition) {
    static constexpr std::array<std::string_view, 16> Suffixes = {"eq",
                                                                  "ne",
                                                                  "cs",
                                                                  "cc",
                                                                  "mi",
                                                                  "pl",
                                                                  "vs",
                                                                  "vc",
                                                                  "hi",
                                                                  "ls",
                                                                  "ge",
                                                                  "lt",
                                                                  "gt",
                                                                  "le",
                                                                  "",
                                                                  "nv"};

    const auto index = static_cast<std::size_t>(condition);
    return index < Suffixes.size() ? Suffixes.at(index) : "??";
}

}  // namespace hexlith::domain::arm
