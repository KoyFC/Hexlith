#include "domain/services/configuration.h"

#include "core/sha256.h"

#include <array>
#include <bit>
#include <format>
#include <string_view>

namespace hexlith::domain::services {
namespace {

// The user name Azahar gives a new console, in UTF-16, ten characters at most.
constexpr std::u16string_view UserName = u"AZAHAR";
constexpr std::size_t UserNameCharacters = 10;
constexpr std::size_t UserNameBlockSize = 0x1C;

// November 7th.
constexpr std::uint8_t BirthdayMonth = 11;
constexpr std::uint8_t BirthdayDay = 7;

// English.
constexpr std::uint8_t English = 1;

// The United States, Washington DC.
constexpr std::uint8_t UnitedStates = 49;
constexpr std::uint8_t WashingtonDc = 2;

constexpr std::uint8_t Stereo = 1;
constexpr std::uint8_t EulaVersion = 0x7F;
constexpr std::uint8_t NintendoTwoDs = 3;

// The other block IDs the table holds. Source: ConfigBlockID in Azahar's cfg.h.
constexpr std::uint32_t UserTimeOffsetBlock = 0x00030001;
constexpr std::uint32_t StereoCameraSettingsBlock = 0x00050005;
constexpr std::uint32_t SoundOutputModeBlock = 0x00070001;
constexpr std::uint32_t ConsoleUniqueId1Block = 0x00090000;
constexpr std::uint32_t ConsoleUniqueId3Block = 0x00090002;
constexpr std::uint32_t UserNameBlock = 0x000A0000;
constexpr std::uint32_t BirthdayBlock = 0x000A0001;
constexpr std::uint32_t CountryInfoBlock = 0x000B0000;
constexpr std::uint32_t CountryNameBlock = 0x000B0001;
constexpr std::uint32_t StateNameBlock = 0x000B0002;
constexpr std::uint32_t LatitudeLongitudeBlock = 0x000B0003;
constexpr std::uint32_t RestrictedPhotoExchangeBlock = 0x000C0000;
constexpr std::uint32_t CoppacsRestrictionBlock = 0x000C0001;
constexpr std::uint32_t ParentalRestrictionEmailBlock = 0x000C0002;
constexpr std::uint32_t EulaVersionBlock = 0x000D0000;
constexpr std::uint32_t UnknownBlock0E = 0x000E0000;
constexpr std::uint32_t DebugModeBlock = 0x00130000;

// Sixteen names of 0x40 UTF-16 characters, one per language.
constexpr std::size_t LocationNames = 0x10;
constexpr std::size_t LocationNameCharacters = 0x40;
constexpr std::u16string_view CountryName = u"United States";
constexpr std::u16string_view StateName = u"Washington DC";

constexpr std::size_t RestrictedPhotoExchangeSize = 0xC0;
constexpr std::size_t CoppacsRestrictionSize = 0x14;
constexpr std::size_t ParentalEmailSize = 0x200;

// The stereo camera settings: eight floats whose meaning nobody has worked out.
// Azahar's values, which it notes fix NaN uniforms in some games.
constexpr std::array<float, 8> StereoCameraSettings = {
    62.0F,
    289.0F,
    76.80000305175781F,
    46.08000183105469F,
    10.0F,
    5.0F,
    55.58000183105469F,
    21.56999969482422F,
};

std::vector<std::uint8_t> FloatsBlock(std::span<const float> values) {
    std::vector<std::uint8_t> block;
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            block.push_back(static_cast<std::uint8_t>(bits >> shift));
        }
    }
    return block;
}

std::vector<std::uint8_t> Bytes(std::initializer_list<std::uint8_t> bytes) {
    return bytes;
}

std::vector<std::uint8_t> Zeros(std::size_t size) {
    return std::vector<std::uint8_t>(size, 0);
}

void PutText(std::vector<std::uint8_t>& block, std::size_t offset, std::u16string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        block.at(offset + (index * 2)) = static_cast<std::uint8_t>(text[index]);
        block.at(offset + (index * 2) + 1) = static_cast<std::uint8_t>(text[index] >> 8U);
    }
}

std::vector<std::uint8_t> UserNameBytes() {
    std::vector<std::uint8_t> block = Zeros(UserNameBlockSize);
    PutText(block, 0, UserName.substr(0, UserNameCharacters));
    return block;
}

std::vector<std::uint8_t> LocationNamesBlock(std::u16string_view name) {
    std::vector<std::uint8_t> block = Zeros(LocationNames * LocationNameCharacters * 2);
    for (std::size_t language = 0; language < LocationNames; ++language) {
        PutText(block, language * LocationNameCharacters * 2, name);
    }
    return block;
}

}  // namespace

Configuration::Configuration() {
    blocks_ = {
        {UserTimeOffsetBlock, Zeros(8)},
        {StereoCameraSettingsBlock, FloatsBlock(StereoCameraSettings)},
        {SoundOutputModeBlock, Bytes({Stereo})},
        {ConsoleUniqueId1Block, Zeros(8)},
        {ConsoleUniqueId2Block, Zeros(8)},
        {ConsoleUniqueId3Block, Zeros(4)},
        {UserNameBlock, UserNameBytes()},
        {BirthdayBlock, Bytes({BirthdayMonth, BirthdayDay})},
        {LanguageBlock, Bytes({English})},
        {CountryInfoBlock, Bytes({0, 0, WashingtonDc, UnitedStates})},
        {CountryNameBlock, LocationNamesBlock(CountryName)},
        {StateNameBlock, LocationNamesBlock(StateName)},
        {LatitudeLongitudeBlock, Zeros(4)},
        {RestrictedPhotoExchangeBlock, Zeros(RestrictedPhotoExchangeSize)},
        {CoppacsRestrictionBlock, Zeros(CoppacsRestrictionSize)},
        {ParentalRestrictionEmailBlock, Zeros(ParentalEmailSize)},
        {EulaVersionBlock, Bytes({EulaVersion, EulaVersion, 0, 0})},
        {UnknownBlock0E, Zeros(1)},
        {ConsoleModelBlock, Bytes({Model, 0, 0, 0})},
        {DebugModeBlock, Zeros(4)},
    };
}

std::span<const Configuration::Command> Configuration::Commands() {
    static constexpr std::array<Command, 10> Table = {{
        {.id = 0x0001, .name = "GetConfig", .handler = &Configuration::GetConfig},
        {.id = 0x0002, .name = "GetRegion", .handler = &Configuration::GetRegion},
        {.id = 0x0003, .name = "GetTransferableId", .handler = &Configuration::GetTransferableId},
        {.id = 0x0004, .name = "IsCoppacsSupported", .handler = &Configuration::IsCoppacsSupported},
        {.id = 0x0005, .name = "GetSystemModel", .handler = &Configuration::GetSystemModel},
        {.id = 0x0006,
         .name = "GetModelNintendo2DS",
         .handler = &Configuration::GetModelNintendo2DS},
        {.id = 0x0008, .name = "TranslateCountryInfo"},
        {.id = 0x0009, .name = "GetCountryCodeString"},
        {.id = 0x000A, .name = "GetCountryCodeID"},
        {.id = 0x000B, .name = "IsFangateSupported"},
    }};
    return Table;
}

const std::vector<std::uint8_t>* Configuration::Block(std::uint32_t id) const {
    const auto found = blocks_.find(id);
    return found == blocks_.end() ? nullptr : &found->second;
}

void Configuration::GetConfig(kernel::IpcRequest& request) {
    const std::uint32_t size = request.Pop();
    const std::uint32_t id = request.Pop();
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    const std::vector<std::uint8_t>* block = Block(id);
    if (block == nullptr || block->size() < size) {
        request.Decline(std::format("configuration block 0x{:08X} of 0x{:X} bytes", id, size));
        return;
    }
    static_cast<void>(request.GetKernel().Space().Memory().Load(
        buffer.address, std::span<const std::uint8_t>(*block).first(size)));
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    request.PushMappedBuffer(buffer);
}

void Configuration::GetRegion(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(Region);
}

void Configuration::GetTransferableId(kernel::IpcRequest& request) {
    // An ID unique to the console and the title: the last eight bytes of the
    // SHA-256 of the console's second unique ID followed by the title's salt,
    // of which the low 20 bits count. Source: GetTransferableId in Azahar's
    // cfg.cpp.
    constexpr std::uint32_t SaltMask = 0x000FFFFF;
    const std::uint32_t salt = request.Pop() & SaltMask;
    std::vector<std::uint8_t> input = *Block(ConsoleUniqueId2Block);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        input.push_back(static_cast<std::uint8_t>(salt >> shift));
    }
    const core::Digest digest = core::Sha256::Of(input);
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |=
            static_cast<std::uint64_t>(digest.bytes.at(digest.bytes.size() - sizeof(value) + byte))
            << (8U * byte);
    }
    request.Begin(3, 0);
    request.Push(kernel::result::Success);
    request.Push64(value);
}

void Configuration::IsCoppacsSupported(kernel::IpcRequest& request) {
    // COPPACS, the parental controls of Canada and the United States, exists
    // only in the region that covers them.
    constexpr std::uint8_t Americas = 1;
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(Region == Americas ? 1U : 0U);
}

void Configuration::GetSystemModel(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(Model);
}

void Configuration::GetModelNintendo2DS(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    // One when the console is not a 2DS.
    request.Push(Model == NintendoTwoDs ? 0U : 1U);
}

}  // namespace hexlith::domain::services
