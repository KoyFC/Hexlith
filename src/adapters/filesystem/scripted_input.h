#ifndef HEXLITH_ADAPTERS_FILESYSTEM_SCRIPTED_INPUT_H
#define HEXLITH_ADAPTERS_FILESYSTEM_SCRIPTED_INPUT_H

#include "application/input_script.h"
#include "ports/input_source.h"

#include <filesystem>
#include <memory>

namespace hexlith::adapters::filesystem {

// Input read from a script file in the format of
// docs/formats/input-script.md, the whole of it read and checked before the
// run starts, so that a mistake on its last line stops nothing halfway.
class ScriptedInput : public ports::InputSource {
public:
    static core::Result<std::unique_ptr<ScriptedInput>> Load(const std::filesystem::path& path);

    [[nodiscard]] domain::services::InputState StateAt(std::uint64_t frame) override;

    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] const application::InputScript& Script() const {
        return script_;
    }

private:
    ScriptedInput(std::filesystem::path path, application::InputScript script)
        : path_(std::move(path)), script_(std::move(script)) {}

    std::filesystem::path path_;
    application::InputScript script_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_SCRIPTED_INPUT_H
