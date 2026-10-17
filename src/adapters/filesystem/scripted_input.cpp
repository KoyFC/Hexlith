#include "adapters/filesystem/scripted_input.h"

#include <format>
#include <fstream>
#include <iterator>
#include <string>

namespace hexlith::adapters::filesystem {

core::Result<std::unique_ptr<ScriptedInput>>
ScriptedInput::Load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return core::Error{std::format("'{}' could not be opened", path.string())};
    }
    const std::string text{std::istreambuf_iterator<char>{file}, {}};
    core::Result<application::InputScript> script = application::InputScript::Parse(text);
    if (!script) {
        return script.Failure().WithContext(path.string());
    }
    // The constructor is private, so make_unique cannot reach it.
    return std::unique_ptr<ScriptedInput>{new ScriptedInput(path, std::move(script.Value()))};
}

domain::services::InputState ScriptedInput::StateAt(std::uint64_t frame) {
    return script_.StateAt(frame);
}

std::string ScriptedInput::Describe() const {
    return path_.string();
}

}  // namespace hexlith::adapters::filesystem
