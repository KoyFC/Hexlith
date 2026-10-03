#include "adapters/filesystem/cartridge_image.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

// The two names a decrypted NCSD cartridge image is given. Hexlith reads the
// container and not the extension, but it has to pick a file before it can
// read anything.
constexpr std::array<std::string_view, 2> ImageExtensions = {".3ds", ".cci"};

std::string Lowercase(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

}  // namespace

core::Result<fs::path> FindCartridgeImage(const fs::path& directory) {
    std::error_code failure;
    if (!fs::is_directory(directory, failure)) {
        return core::Error{std::format("'{}' is not a directory", directory.string())};
    }

    std::vector<fs::path> candidates;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, failure}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = Lowercase(entry.path().extension().string());
        if (std::ranges::find(ImageExtensions, extension) != ImageExtensions.end()) {
            candidates.push_back(entry.path());
        }
    }
    if (failure) {
        return core::Error{
            std::format("'{}' could not be listed: {}", directory.string(), failure.message())};
    }

    if (candidates.empty()) {
        return core::Error{std::format(
            "'{}' holds no .3ds or .cci file. Name the image with --image if it is elsewhere",
            directory.string())};
    }

    // Sorted so that the message reads the same way twice, and so that the
    // single-candidate case cannot depend on directory order either.
    std::ranges::sort(candidates);

    if (candidates.size() > 1) {
        std::string message =
            std::format("'{}' holds {} candidate images, so --image has to say which",
                        directory.string(),
                        candidates.size());
        for (const fs::path& candidate : candidates) {
            message.append("\n  ");
            message.append(candidate.filename().string());
        }
        return core::Error{std::move(message)};
    }

    return candidates.front();
}
}  // namespace hexlith::adapters::filesystem
