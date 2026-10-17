#include "application/input_script.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace hexlith::application {
namespace {

namespace services = domain::services;
namespace button = domain::services::button;

// How long a tap or a touch lasts when the line does not say. Four frames is
// long enough for a game that reads its input every other frame, as many
// scenes at thirty frames a second do, to see both the press and the release.
constexpr std::uint64_t DefaultHoldFrames = 4;

// The bottom screen, which touch coordinates are in. Source: 3dbrew, LCD.
constexpr std::uint32_t BottomScreenWidth = 320;
constexpr std::uint32_t BottomScreenHeight = 240;

// How far the circle pad goes from its centre. Source: MAX_CIRCLEPAD_POS in
// Azahar's hid.cpp, which notes it was measured with a calibrated pad.
constexpr std::int32_t CircleRim = 0x9A;

constexpr std::array<std::pair<std::string_view, std::uint32_t>, 12> ButtonNames = {{
    {"a", button::A},
    {"b", button::B},
    {"x", button::X},
    {"y", button::Y},
    {"l", button::L},
    {"r", button::R},
    {"start", button::Start},
    {"select", button::Select},
    {"up", button::Up},
    {"down", button::Down},
    {"left", button::Left},
    {"right", button::Right},
}};

// One change to the input, at a frame. Order breaks ties between changes at
// the same frame: the line it came from, so that what a later line says wins.
struct Change {
    enum class Kind : std::uint8_t { Press, Release, Touch, Lift, Circle };
    std::uint64_t frame = 0;
    std::size_t order = 0;
    Kind kind = Kind::Press;
    std::uint32_t buttons = 0;
    services::TouchPoint point;
    std::int16_t circle_x = 0;
    std::int16_t circle_y = 0;
};

std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t at = 0;
    while (at < line.size()) {
        const std::size_t start = line.find_first_not_of(" \t\r", at);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(line.find_first_of(" \t\r", start), line.size());
        tokens.push_back(line.substr(start, end - start));
        at = end;
    }
    return tokens;
}

// A whole number, optionally negative, parsed by hand as the analysis
// tables parse theirs. Anything larger than a frame count needs is refused.
std::optional<std::int64_t> ParseNumber(std::string_view text) {
    constexpr std::int64_t Decimal = 10;
    constexpr std::int64_t Limit = 1'000'000'000'000;
    const bool negative = text.starts_with('-');
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = (value * Decimal) + (digit - '0');
        if (value > Limit) {
            return std::nullopt;
        }
    }
    return negative ? -value : value;
}

std::optional<std::uint32_t> ButtonNamed(std::string_view name) {
    for (const auto& [known, mask] : ButtonNames) {
        if (known == name) {
            return mask;
        }
    }
    return std::nullopt;
}

// A line being read: its tokens, which of them comes next, and where it is
// for a complaint.
class LineReader {
public:
    LineReader(std::vector<std::string_view> tokens, std::size_t number)
        : tokens_(std::move(tokens)), number_(number) {}

    [[nodiscard]] bool Done() const {
        return next_ >= tokens_.size();
    }

    [[nodiscard]] std::string_view Peek() const {
        return Done() ? std::string_view{} : tokens_.at(next_);
    }

    std::string_view Take() {
        const std::string_view token = Peek();
        ++next_;
        return token;
    }

    core::Result<std::int64_t> Number(std::string_view what, std::int64_t low, std::int64_t high) {
        if (Done()) {
            return Error(std::format("{} is missing", what));
        }
        const std::string_view token = Take();
        const std::optional<std::int64_t> value = ParseNumber(token);
        if (!value.has_value() || *value < low || *value > high) {
            return Error(
                std::format("{} '{}' is not a number from {} to {}", what, token, low, high));
        }
        return *value;
    }

    // Button names up to the end of the line or the word "for".
    core::Result<std::uint32_t> Buttons() {
        std::uint32_t buttons = 0;
        while (!Done() && Peek() != "for") {
            const std::string_view name = Take();
            const std::optional<std::uint32_t> mask = ButtonNamed(name);
            if (!mask.has_value()) {
                return Error(std::format("'{}' is not a button", name));
            }
            buttons |= *mask;
        }
        if (buttons == 0) {
            return Error("no button is named");
        }
        return buttons;
    }

    // "for <frames>", if the line has it.
    core::Result<std::uint64_t> Length() {
        if (Done()) {
            return DefaultHoldFrames;
        }
        if (Take() != "for") {
            return Error("only 'for <frames>' may follow");
        }
        const core::Result<std::int64_t> frames = Number("the length", 1, 1'000'000);
        if (!frames) {
            return frames.Failure();
        }
        return static_cast<std::uint64_t>(frames.Value());
    }

    [[nodiscard]] core::Status End() const {
        if (!Done()) {
            return Error(std::format("'{}' is not expected here", Peek()));
        }
        return {};
    }

    [[nodiscard]] core::Error Error(std::string_view message) const {
        return core::Error{std::format("line {}: {}", number_, message)};
    }

private:
    std::vector<std::string_view> tokens_;
    std::size_t number_;
    std::size_t next_ = 0;
};

core::Result<services::TouchPoint> Point(LineReader& line) {
    const core::Result<std::int64_t> x = line.Number("x", 0, BottomScreenWidth - 1);
    if (!x) {
        return x.Failure();
    }
    const core::Result<std::int64_t> y = line.Number("y", 0, BottomScreenHeight - 1);
    if (!y) {
        return y.Failure();
    }
    return services::TouchPoint{.x = static_cast<std::uint16_t>(x.Value()),
                                .y = static_cast<std::uint16_t>(y.Value())};
}

// The changes one line makes, after its frame.
core::Status
ReadAction(LineReader& line, std::uint64_t frame, std::size_t order, std::vector<Change>& changes) {
    const std::string_view action = line.Take();
    Change change;
    change.frame = frame;
    change.order = order;

    if (action == "press" || action == "release" || action == "tap") {
        const core::Result<std::uint32_t> buttons = line.Buttons();
        if (!buttons) {
            return buttons.Failure();
        }
        change.buttons = buttons.Value();
        change.kind = action == "release" ? Change::Kind::Release : Change::Kind::Press;
        changes.push_back(change);
        if (action != "tap") {
            return line.End();
        }
        const core::Result<std::uint64_t> length = line.Length();
        if (!length) {
            return length.Failure();
        }
        change.kind = Change::Kind::Release;
        change.frame = frame + length.Value();
        changes.push_back(change);
        return line.End();
    }

    if (action == "touch") {
        const core::Result<services::TouchPoint> point = Point(line);
        if (!point) {
            return point.Failure();
        }
        change.kind = Change::Kind::Touch;
        change.point = point.Value();
        changes.push_back(change);
        // Without a length the stylus stays down until a lift.
        if (line.Done()) {
            return {};
        }
        const core::Result<std::uint64_t> length = line.Length();
        if (!length) {
            return length.Failure();
        }
        change.kind = Change::Kind::Lift;
        change.frame = frame + length.Value();
        changes.push_back(change);
        return line.End();
    }

    if (action == "lift") {
        change.kind = Change::Kind::Lift;
        changes.push_back(change);
        return line.End();
    }

    if (action == "circle") {
        const core::Result<std::int64_t> x = line.Number("x", -CircleRim, CircleRim);
        if (!x) {
            return x.Failure();
        }
        const core::Result<std::int64_t> y = line.Number("y", -CircleRim, CircleRim);
        if (!y) {
            return y.Failure();
        }
        change.kind = Change::Kind::Circle;
        change.circle_x = static_cast<std::int16_t>(x.Value());
        change.circle_y = static_cast<std::int16_t>(y.Value());
        changes.push_back(change);
        return line.End();
    }

    return line.Error(std::format("'{}' is not an action", action));
}

void Apply(const Change& change, services::InputState& state) {
    switch (change.kind) {
    case Change::Kind::Press:
        state.buttons |= change.buttons;
        break;
    case Change::Kind::Release:
        state.buttons &= ~change.buttons;
        break;
    case Change::Kind::Touch:
        state.touch = change.point;
        break;
    case Change::Kind::Lift:
        state.touch.reset();
        break;
    case Change::Kind::Circle:
        state.circle_x = change.circle_x;
        state.circle_y = change.circle_y;
        break;
    }
}

}  // namespace

core::Result<InputScript> InputScript::Parse(std::string_view text) {
    std::vector<Change> changes;
    std::uint64_t last_frame = 0;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        line = line.substr(0, line.find('#'));

        LineReader reader{Tokens(line), number};
        if (reader.Done()) {
            continue;
        }
        const core::Result<std::int64_t> frame =
            reader.Number("the frame", 0, std::numeric_limits<std::int32_t>::max());
        if (!frame) {
            return frame.Failure();
        }
        const auto at = static_cast<std::uint64_t>(frame.Value());
        if (at < last_frame) {
            return reader.Error(
                std::format("frame {} comes before frame {}, on an earlier line", at, last_frame));
        }
        last_frame = at;
        if (reader.Done()) {
            return reader.Error("no action follows the frame");
        }
        const core::Status read = ReadAction(reader, at, number, changes);
        if (!read) {
            return read.Failure();
        }
    }

    std::ranges::stable_sort(changes, [](const Change& left, const Change& right) {
        return std::pair{left.frame, left.order} < std::pair{right.frame, right.order};
    });

    std::vector<Snapshot> snapshots;
    services::InputState state;
    for (const Change& change : changes) {
        Apply(change, state);
        if (!snapshots.empty() && snapshots.back().frame == change.frame) {
            snapshots.back().state = state;
        } else {
            snapshots.push_back(Snapshot{.frame = change.frame, .state = state});
        }
    }
    return InputScript{std::move(snapshots)};
}

services::InputState InputScript::StateAt(std::uint64_t frame) const {
    const auto after = std::ranges::upper_bound(snapshots_, frame, {}, &Snapshot::frame);
    if (after == snapshots_.begin()) {
        return {};
    }
    return std::prev(after)->state;
}

std::uint64_t InputScript::LastFrame() const {
    return snapshots_.empty() ? 0 : snapshots_.back().frame;
}

}  // namespace hexlith::application
