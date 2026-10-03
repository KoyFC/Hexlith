#ifndef HEXLITH_DOMAIN_CTR_LAYOUT_H
#define HEXLITH_DOMAIN_CTR_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

// Where each field of the CTR container formats sits, and how large it is.
//
// One definition serves the parsers and the synthetic image builder the tests
// use, so that the two cannot disagree about where a field lives. The builder
// writing a field somewhere the parser does not read it would otherwise
// produce a test suite that passes while the tool fails on a real image.
//
// Every value here is documented, with its source, in docs/formats/. Nothing
// in this file is inferred from the reference image.
namespace hexlith::domain::ctr {

// A media unit. Every offset and size in an NCSD partition table and in the
// NCCH region fields is expressed in these rather than in bytes.
// Source: 3dbrew NCSD.
constexpr std::size_t MediaUnit = 0x200;

constexpr std::size_t BytesFromMediaUnits(std::uint32_t units) {
    return static_cast<std::size_t>(units) * MediaUnit;
}

// The RSA-2048 signature every container header carries, and which Hexlith
// never verifies. It is skipped rather than read.
// Source: 3dbrew NCSD, NCCH.
constexpr std::size_t SignatureSize = 0x100;

constexpr std::size_t Sha256Size = 0x20;

// NCSD, the cartridge image. Source: docs/formats/ncsd.md, 3dbrew NCSD.
namespace ncsd {

constexpr std::string_view Magic = "NCSD";
constexpr std::size_t MagicOffset = 0x100;
constexpr std::size_t ImageSizeOffset = 0x104;
constexpr std::size_t MediaIdOffset = 0x108;
constexpr std::size_t PartitionFsTypesOffset = 0x110;
constexpr std::size_t PartitionCryptTypesOffset = 0x118;
constexpr std::size_t PartitionTableOffset = 0x120;
constexpr std::size_t ExHeaderHashOffset = 0x160;
constexpr std::size_t AdditionalHeaderSizeOffset = 0x180;
constexpr std::size_t SectorZeroOffsetOffset = 0x184;
constexpr std::size_t PartitionFlagsOffset = 0x188;
constexpr std::size_t PartitionIdTableOffset = 0x190;
constexpr std::size_t HeaderSize = 0x200;

// Eight pairs of offset and length, each a 32-bit count of media units.
constexpr std::size_t PartitionCount = 8;
constexpr std::size_t PartitionTableEntrySize = 8;

// The partition the executable content of a game lives in. Hexlith takes the
// index from the GameProfile rather than scanning, so this is the value a
// profile is expected to name and not a default the code falls back on.
constexpr std::size_t ExecutableContentPartition = 0;

// The Card Info Header, present in a cartridge image immediately after the
// NCSD header. Source: docs/formats/ncsd.md.
namespace card_info {

constexpr std::size_t Offset = 0x200;
constexpr std::size_t WritableAddressOffset = 0x200;
constexpr std::size_t CardInfoBitmaskOffset = 0x204;
constexpr std::size_t FilledSizeOffset = 0x300;
constexpr std::size_t TitleVersionOffset = 0x310;
constexpr std::size_t CardRevisionOffset = 0x312;
constexpr std::size_t CverTitleIdOffset = 0x320;
constexpr std::size_t CverVersionOffset = 0x328;
constexpr std::size_t InitialDataOffset = 0x1000;
constexpr std::size_t InitialDataSize = 0x200;
constexpr std::size_t EndOffset = 0x1200;

}  // namespace card_info
}  // namespace ncsd

// NCCH, one content partition. Every region offset below is relative to the
// start of the NCCH and not to the start of the file.
// Source: docs/formats/ncch.md, 3dbrew NCCH.
namespace ncch {

constexpr std::string_view Magic = "NCCH";
constexpr std::size_t MagicOffset = 0x100;
constexpr std::size_t ContentSizeOffset = 0x104;
constexpr std::size_t PartitionIdOffset = 0x108;
constexpr std::size_t MakerCodeOffset = 0x110;
constexpr std::size_t VersionOffset = 0x112;
constexpr std::size_t ContentLockSeedOffset = 0x114;
constexpr std::size_t ProgramIdOffset = 0x118;
constexpr std::size_t LogoHashOffset = 0x130;
constexpr std::size_t ProductCodeOffset = 0x150;
constexpr std::size_t ProductCodeSize = 0x10;
constexpr std::size_t ExHeaderHashOffset = 0x160;
constexpr std::size_t ExHeaderSizeOffset = 0x180;
constexpr std::size_t FlagsOffset = 0x188;
constexpr std::size_t FlagsSize = 8;
constexpr std::size_t PlainRegionOffsetOffset = 0x190;
constexpr std::size_t PlainRegionSizeOffset = 0x194;
constexpr std::size_t LogoRegionOffsetOffset = 0x198;
constexpr std::size_t LogoRegionSizeOffset = 0x19C;
constexpr std::size_t ExeFsOffsetOffset = 0x1A0;
constexpr std::size_t ExeFsSizeOffset = 0x1A4;
constexpr std::size_t ExeFsHashRegionSizeOffset = 0x1A8;

// The reserved word at 0x1AC sits between the ExeFS group and the RomFS
// group. Reading the RomFS fields four bytes late yields a hash region size
// and a zero, which looks like a title with no RomFS. docs/formats/ncch.md
// records that this mistake was made and caught.
constexpr std::size_t RomFsOffsetOffset = 0x1B0;
constexpr std::size_t RomFsSizeOffset = 0x1B4;
constexpr std::size_t RomFsHashRegionSizeOffset = 0x1B8;
constexpr std::size_t ExeFsSuperblockHashOffset = 0x1C0;
constexpr std::size_t RomFsSuperblockHashOffset = 0x1E0;
constexpr std::size_t HeaderSize = 0x200;

// Flags byte 7 is a bitmask. Hexlith requires NoCrypto and refuses anything
// else, since the project handles no keys.
constexpr std::size_t CryptoMethodByte = 3;
constexpr std::size_t ContentPlatformByte = 4;
constexpr std::size_t ContentTypeByte = 5;
constexpr std::size_t ContentUnitSizeByte = 6;
constexpr std::size_t BitmaskByte = 7;

constexpr std::uint8_t FixedCryptoKeyFlag = 0x01;
constexpr std::uint8_t NoMountRomFsFlag = 0x02;
constexpr std::uint8_t NoCryptoFlag = 0x04;

// Bits 0 and 1 of the content type byte are the content form, bits 2 to 7
// the content type. The executable bit is bit 1 of that byte.
constexpr std::uint8_t ExecutableContentFlag = 0x02;
constexpr std::uint8_t DataContentFlag = 0x01;

constexpr std::uint8_t ContentPlatformCtr = 1;
constexpr std::uint8_t ContentPlatformNew3ds = 2;

}  // namespace ncch

// The NCCH extended header, immediately after the NCCH header.
// Source: docs/formats/exheader.md, 3dbrew NCCH/Extended_Header.
namespace exheader {

// The NCCH header's size field reports 0x400, which is the region its hash
// covers. The region on disk is 0x800: the second half is the access
// descriptor, which carries a signature over the permissions the title asks
// for. docs/formats/exheader.md measures both.
constexpr std::size_t HashedSize = 0x400;
constexpr std::size_t SizeOnDisk = 0x800;

// System Control Info, at the start.
constexpr std::size_t ApplicationTitleOffset = 0x00;
constexpr std::size_t ApplicationTitleSize = 8;
constexpr std::size_t FlagsOffset = 0x0D;
constexpr std::size_t RemasterVersionOffset = 0x0E;
constexpr std::size_t TextCodeSetOffset = 0x10;
constexpr std::size_t StackSizeOffset = 0x1C;
constexpr std::size_t ReadOnlyCodeSetOffset = 0x20;
constexpr std::size_t DataCodeSetOffset = 0x30;
constexpr std::size_t BssSizeOffset = 0x3C;
constexpr std::size_t DependencyListOffset = 0x40;
constexpr std::size_t DependencyCount = 48;
constexpr std::size_t SystemInfoOffset = 0x1C0;
constexpr std::size_t SaveDataSizeOffset = 0x1C0;
constexpr std::size_t JumpIdOffset = 0x1C8;

// A code set entry: the virtual address the segment loads at, the size of
// its physical region in pages, and its size in bytes.
constexpr std::size_t CodeSetEntrySize = 0x0C;
constexpr std::size_t CodeSetAddressOffset = 0x0;
constexpr std::size_t CodeSetPageCountOffset = 0x4;
constexpr std::size_t CodeSetSizeOffset = 0x8;

constexpr std::size_t PageSize = 0x1000;

// Bit 0 of the flags byte is the only thing that says the ExeFS .code
// section is compressed. Nothing in the ExeFS itself indicates it.
constexpr std::uint8_t CompressExefsCodeFlag = 0x01;
constexpr std::uint8_t SdApplicationFlag = 0x02;

// Access Control Info, at 0x200. Offsets below are from the start of the
// extended header, not from the start of the ACI.
constexpr std::size_t AccessControlInfoOffset = 0x200;
constexpr std::size_t AciProgramIdOffset = 0x200;
constexpr std::size_t AciCoreVersionOffset = 0x208;
constexpr std::size_t AciFlag1Offset = 0x20C;
constexpr std::size_t AciFlag2Offset = 0x20D;
constexpr std::size_t AciFlag0Offset = 0x20E;
constexpr std::size_t AciPriorityOffset = 0x20F;
constexpr std::size_t AciResourceLimitsOffset = 0x210;
constexpr std::size_t AciStorageInfoOffset = 0x230;

// The service access control list is the authoritative statement of which
// Horizon services the title can reach, which makes it the work list for the
// HLE layer.
constexpr std::size_t ServiceAccessControlOffset = 0x250;
constexpr std::size_t ServiceAccessControlCount = 34;
constexpr std::size_t ServiceNameSize = 8;
constexpr std::size_t ExtendedServiceAccessControlOffset = 0x350;
constexpr std::size_t ResourceLimitCategoryOffset = 0x36F;

// The access descriptor, which is the half the NCCH header's size field does
// not account for.
constexpr std::size_t AccessDescriptorOffset = 0x400;
constexpr std::size_t AccessDescriptorSignatureSize = 0x100;
constexpr std::size_t NcchPublicKeyOffset = 0x500;
constexpr std::size_t NcchPublicKeySize = 0x100;
constexpr std::size_t LimitationAciOffset = 0x600;

}  // namespace exheader

// ExeFS. Source: docs/formats/exefs.md, 3dbrew ExeFS.
namespace exefs {

constexpr std::size_t HeaderSize = 0x200;

// Up to ten entries of 0x10 bytes. Azahar's extractor, and the script
// derived from it, iterate eight. That is harmless in practice because
// .code is always first, but it is wrong against the format.
constexpr std::size_t MaxFileCount = 10;
constexpr std::size_t FileHeaderOffset = 0x000;
constexpr std::size_t FileHeaderSize = 0x10;
constexpr std::size_t FileNameSize = 8;
constexpr std::size_t FileOffsetOffset = 0x8;
constexpr std::size_t FileSizeOffset = 0xC;
constexpr std::size_t ReservedOffset = 0x0A0;

// The hash table is stored in reverse order against the file headers, so the
// hash at 0x1E0 belongs to the first file and the one at 0x0C0 to the tenth.
constexpr std::size_t HashTableOffset = 0x0C0;

constexpr std::string_view CodeFileName = ".code";
constexpr std::string_view BannerFileName = "banner";
constexpr std::string_view IconFileName = "icon";

// The backwards-LZSS footer, in the last eight bytes of a compressed
// section. The high byte of the first word is the footer size and the low
// three bytes are the encoded size; the second word, added to the
// compressed length, gives the decompressed length.
constexpr std::size_t CompressionFooterSize = 8;
constexpr std::uint32_t EncodedSizeMask = 0x00FFFFFF;
constexpr unsigned FooterSizeShift = 24;

}  // namespace exefs

// RomFS, the title's read-only data filesystem, wrapped in an IVFC hash tree.
// Source: docs/formats/romfs.md, 3dbrew RomFS, and
// azahar/src/core/file_sys/ncch_container.cpp for the level 3 offset.
namespace romfs {

constexpr std::string_view IvfcMagic = "IVFC";
constexpr std::size_t IvfcMagicOffset = 0x00;
constexpr std::size_t IvfcMagicNumberOffset = 0x04;
constexpr std::uint32_t IvfcMagicNumber = 0x10000;
constexpr std::size_t MasterHashSizeOffset = 0x08;
constexpr std::size_t Level1LogicalOffsetOffset = 0x0C;
constexpr std::size_t Level2LogicalOffsetOffset = 0x24;
constexpr std::size_t Level3LogicalOffsetOffset = 0x3C;
constexpr std::size_t OptionalInfoSizeOffset = 0x58;

// The level 3 filesystem begins 0x1000 bytes into the RomFS region, which
// Azahar hardcodes and this project confirmed by observation.
//
// The logical offsets in the IVFC header are not file offsets. They address
// the hash level space: on the reference image level 3 reports 0xD66000,
// where the header length field reads 0x1E7F5 and nothing parses. Using one
// as a file offset produces a failure that looks like a corrupt image rather
// than like a wrong constant.
constexpr std::size_t Level3Offset = 0x1000;

// The level 3 header. Every offset in it is relative to the start of level 3.
constexpr std::uint32_t Level3HeaderLength = 0x28;
constexpr std::size_t HeaderLengthOffset = 0x00;
constexpr std::size_t DirectoryHashTableOffsetOffset = 0x04;
constexpr std::size_t DirectoryHashTableLengthOffset = 0x08;
constexpr std::size_t DirectoryMetadataOffsetOffset = 0x0C;
constexpr std::size_t DirectoryMetadataLengthOffset = 0x10;
constexpr std::size_t FileHashTableOffsetOffset = 0x14;
constexpr std::size_t FileHashTableLengthOffset = 0x18;
constexpr std::size_t FileMetadataOffsetOffset = 0x1C;
constexpr std::size_t FileMetadataLengthOffset = 0x20;
constexpr std::size_t FileDataOffsetOffset = 0x24;

// A directory metadata entry, followed by its name.
constexpr std::size_t DirectoryParentOffset = 0x00;
constexpr std::size_t DirectoryNextSiblingOffset = 0x04;
constexpr std::size_t DirectoryFirstChildOffset = 0x08;
constexpr std::size_t DirectoryFirstFileOffset = 0x0C;
constexpr std::size_t DirectoryNextInBucketOffset = 0x10;
constexpr std::size_t DirectoryNameLengthOffset = 0x14;
constexpr std::size_t DirectoryEntrySize = 0x18;

// A file metadata entry, followed by its name.
constexpr std::size_t FileParentOffset = 0x00;
constexpr std::size_t FileNextSiblingOffset = 0x04;
constexpr std::size_t FileDataOffsetField = 0x08;
constexpr std::size_t FileDataLengthField = 0x10;
constexpr std::size_t FileNextInBucketOffset = 0x18;
constexpr std::size_t FileNameLengthOffset = 0x1C;
constexpr std::size_t FileEntrySize = 0x20;

// Terminates a sibling chain or a hash bucket.
constexpr std::uint32_t NoEntry = 0xFFFFFFFF;

// A name is padded to a multiple of four bytes.
constexpr std::size_t NameAlignment = 4;

// The root directory is the entry at offset zero of the directory metadata
// table, and it has no name.
constexpr std::uint32_t RootDirectory = 0;

}  // namespace romfs

// CRO, a dynamically loaded code module, and the CRS that describes the main
// executable. Source: docs/formats/cro0.md, 3dbrew CRO0, and
// azahar/src/core/hle/service/ldr_ro/cro_helper.h for the field layout.
namespace cro {

constexpr std::string_view Magic = "CRO0";

// A module the loader has already relocated reports this instead. Hexlith
// only ever reads modules straight out of the RomFS, so finding it would mean
// reading something that had been through a console.
constexpr std::string_view FixedUpMagic = "FIXD";

// Four SHA-256 hashes covering different regions of the module. Hexlith does
// not verify them; the CRR exists to check them and the project checks
// nothing.
constexpr std::size_t HashesSize = 0x80;

// From 0x80 the header is a flat array of 32-bit fields, so the field at
// index n sits at 0x80 + 4n. docs/formats/cro0.md carries the index of every
// one, because the table offsets and counts alternate and a pair is easy to
// slip by one. That mistake was made during the initial survey.
constexpr std::size_t FieldArrayOffset = 0x80;
constexpr std::size_t FieldSize = 4;

constexpr std::size_t FieldOffset(std::size_t index) {
    return FieldArrayOffset + (index * FieldSize);
}

constexpr std::size_t MagicField = 0;
constexpr std::size_t NameOffsetField = 1;
constexpr std::size_t NextModuleField = 2;
constexpr std::size_t PreviousModuleField = 3;
constexpr std::size_t FileSizeField = 4;
constexpr std::size_t BssSizeField = 5;
constexpr std::size_t FixedSizeField = 6;
constexpr std::size_t OnLoadSegmentTagField = 9;
constexpr std::size_t OnExitSegmentTagField = 10;
constexpr std::size_t OnUnresolvedSegmentTagField = 11;
constexpr std::size_t CodeOffsetField = 12;
constexpr std::size_t CodeSizeField = 13;
constexpr std::size_t DataOffsetField = 14;
constexpr std::size_t DataSizeField = 15;
constexpr std::size_t ModuleNameOffsetField = 16;
constexpr std::size_t ModuleNameSizeField = 17;
constexpr std::size_t SegmentTableOffsetField = 18;
constexpr std::size_t SegmentCountField = 19;
constexpr std::size_t NamedExportTableOffsetField = 20;
constexpr std::size_t NamedExportCountField = 21;
constexpr std::size_t IndexedExportTableOffsetField = 22;
constexpr std::size_t IndexedExportCountField = 23;
constexpr std::size_t ExportStringsOffsetField = 24;
constexpr std::size_t ExportStringsSizeField = 25;
constexpr std::size_t ExportTreeTableOffsetField = 26;
constexpr std::size_t ExportTreeNodeCountField = 27;
constexpr std::size_t ImportModuleTableOffsetField = 28;
constexpr std::size_t ImportModuleCountField = 29;

// The field at index 30 is the external relocation table, not an import
// table. It was labelled as one during the initial survey and the numbers
// reported before that was caught were wrong.
constexpr std::size_t ExternalRelocationTableOffsetField = 30;
constexpr std::size_t ExternalRelocationCountField = 31;
constexpr std::size_t NamedImportTableOffsetField = 32;
constexpr std::size_t NamedImportCountField = 33;
constexpr std::size_t IndexedImportTableOffsetField = 34;
constexpr std::size_t IndexedImportCountField = 35;
constexpr std::size_t AnonymousImportTableOffsetField = 36;
constexpr std::size_t AnonymousImportCountField = 37;
constexpr std::size_t ImportStringsOffsetField = 38;
constexpr std::size_t ImportStringsSizeField = 39;
constexpr std::size_t StaticAnonymousSymbolTableOffsetField = 40;
constexpr std::size_t StaticAnonymousSymbolCountField = 41;
constexpr std::size_t InternalRelocationTableOffsetField = 42;
constexpr std::size_t InternalRelocationCountField = 43;
constexpr std::size_t StaticRelocationTableOffsetField = 44;
constexpr std::size_t StaticRelocationCountField = 45;
constexpr std::size_t FixLevel0BarrierField = 46;

constexpr std::size_t FieldCount = 47;
constexpr std::size_t HeaderSize = FieldOffset(FieldCount);

// A segment table entry: an offset, a size and a type.
constexpr std::size_t SegmentEntrySize = 12;
constexpr std::size_t SegmentOffsetOffset = 0x0;
constexpr std::size_t SegmentSizeOffset = 0x4;
constexpr std::size_t SegmentTypeOffset = 0x8;

// A named export entry: an offset into the export string table, and the
// symbol's position within its own segment.
constexpr std::size_t NamedExportEntrySize = 8;
constexpr std::size_t NamedExportNameOffsetOffset = 0x0;
constexpr std::size_t NamedExportPositionOffset = 0x4;

// The module name of the CRS, with the pipe characters included, which is how
// the loader tells it apart from a loadable module. Its segment offsets are
// absolute virtual addresses rather than file offsets, because the segments
// it describes are already mapped.
constexpr std::string_view StaticModuleName = "|static|";

}  // namespace cro
}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_LAYOUT_H
