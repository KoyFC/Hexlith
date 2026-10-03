#ifndef HEXLITH_TESTS_SUPPORT_SYNTHETIC_ROMFS_H
#define HEXLITH_TESTS_SUPPORT_SYNTHETIC_ROMFS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Builds a RomFS region in memory: an IVFC header, a level 3 filesystem with
// its metadata tables, and a file data area.
//
// The hash tables are written with the right sizes and filled with the empty
// bucket marker. The parser does not read them, and writing real ones would
// mean implementing a lookup nobody uses.
//
// The IVFC level 3 logical offset is deliberately written as a value that is
// not a file offset, the way the reference image has it. A parser that used
// it instead of the fixed 0x1000 would then fail the tests rather than work
// against the fixture and break against the cartridge.
namespace hexlith::test {

struct SyntheticRomFsFile {
    // Relative, with forward slashes. Intermediate directories are created
    // as needed.
    std::string path;
    std::vector<std::uint8_t> data;
};

struct SyntheticRomFsSpec {
    std::vector<SyntheticRomFsFile> files;

    // Directories with nothing in them, which no list of file paths can
    // express.
    std::vector<std::string> empty_directories;
};

std::vector<std::uint8_t> BuildSyntheticRomFs(const SyntheticRomFsSpec& spec);

// A filesystem shaped like the one on the cartridge: modules and the static
// module in the root, bulk data under a numbered hierarchy, and a name that
// is not ASCII so that the UTF-16 names the format stores are exercised.
SyntheticRomFsSpec DefaultRomFsSpec();

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_SYNTHETIC_ROMFS_H
