#pragma once

// The image library's files on disk: one file per stored image, and one list of the
// images in each set. This is the format and the rules that name a folder; writing
// the files is image_cache_writer.h, and reading them back is image_cache_reader.h.
//
// No engine, no D3D and no file I/O here, so every rule is testable with plain bytes.
//
// An image file, all numbers little-endian:
//
//   u32  magic             'SRIC'
//   u32  revision          kRevision
//   u32  sourceFormat      the game image's DXGI_FORMAT
//   u32  sourceWidth       level 0 of the game image
//   u32  sourceHeight
//   u32  storedWidth       level 0 of the stored copy
//   u32  storedHeight
//   u32  levels            LevelCount(storedWidth, storedHeight)
//   u32  pathLength        bytes of the source object path that follows the header
//   u64  payloadBytes      ImageShrink::Bytes(stored size, levels)
//   u32  crc32             of the header up to here, then the path, then the payload
//   ...  path              UTF-8, no terminator
//   ...  payload           8-bit RGBA, every level tightly packed, level 0 first
//
// A set list:
//
//   u32  magic             'SRSL'
//   u32  revision          kRevision
//   u32  setCount
//   per set: u32 set (a PluginImageSet bit), u32 pathCount,
//            per path: u32 length, then the UTF-8 bytes
//   u32  crc32             of everything before it
//
// A name list (the item names that resolved to a texture path):
//
//   u32  magic             'SRNL'
//   u32  revision          kRevision
//   u32  pairCount
//   per pair: u32 length, the UTF-8 name, u32 length, the UTF-8 texture path
//   u32  crc32             of everything before it

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ImageCache
{
	inline constexpr uint32_t kImageMagic   = 0x43495253;   // "SRIC"
	inline constexpr uint32_t kSetListMagic = 0x4C535253;   // "SRSL"
	inline constexpr uint32_t kNameListMagic = 0x4C4E5253;  // "SRNL"

	// Bumped whenever either file layout or the stored pixels change, or the lists
	// saved in the folder were made by code that no longer builds them the same way
	// (a set list that leaves out images). It is part of the folder key, so a new
	// revision starts a new folder.
	inline constexpr uint32_t kRevision = 3;

	inline constexpr uint32_t kHeaderBytes   = 48;
	inline constexpr uint32_t kMaxPathLength = 1024;
	inline constexpr uint32_t kMaxLevels     = 16;
	inline constexpr uint32_t kMaxSide       = 4096;
	inline constexpr uint32_t kMaxSetPaths   = 65536;
	inline constexpr uint32_t kMaxSets       = 32;
	inline constexpr uint32_t kMaxNames      = 65536;

	enum class Parsed
	{
		Ok,
		TooShort,        // not even a header
		BadMagic,
		WrongRevision,
		BadHeader,       // a field is out of range or disagrees with another
		Truncated,       // the file ends early, or has bytes after the payload
		BadCrc,
	};

	struct Info
	{
		uint32_t    sourceFormat = 0;
		uint32_t    sourceWidth  = 0;
		uint32_t    sourceHeight = 0;
		uint32_t    storedWidth  = 0;
		uint32_t    storedHeight = 0;
		uint32_t    levels       = 0;
		std::string path;
	};

	uint32_t Crc32(const void* data, size_t size, uint32_t seed = 0);

	// The bytes of an image file for the pixels of `info`, or empty when info is not valid or
	// pixelBytes is not the size info asks for.
	std::vector<uint8_t> Serialize(const Info& info, const uint8_t* pixels, size_t pixelBytes);

	// Checks the file in data and, on Ok, fills info and points pixels into data (the payload).
	Parsed Parse(const uint8_t* data, size_t size, Info& info, const uint8_t*& pixels);

	const char* Describe(Parsed result);

	// ---- Set lists ----------------------------------------------------------

	struct SetEntry
	{
		uint32_t                 set = 0;
		std::vector<std::string> paths;

		bool operator==(const SetEntry& other) const { return set == other.set && paths == other.paths; }
	};
	using SetList = std::vector<SetEntry>;

	std::vector<uint8_t> SerializeSets(const SetList& sets);
	Parsed ParseSets(const uint8_t* data, size_t size, SetList& sets);

	// ---- Name lists ---------------------------------------------------------

	struct NamePair
	{
		std::string name;   // as a plugin asks for it
		std::string path;   // the texture's object path, normalised

		bool operator==(const NamePair& other) const { return name == other.name && path == other.path; }
	};
	using NameList = std::vector<NamePair>;

	std::vector<uint8_t> SerializeNames(const NameList& names);
	Parsed ParseNames(const uint8_t* data, size_t size, NameList& names);

	// ---- Names --------------------------------------------------------------

	// The file an image is kept in: 16 hex digits of a hash of its normalised object path,
	// then ".img". The header holds the path itself, so a collision is caught on read.
	std::string FileName(const std::string& normalizedPath);

	// The name of the set list.
	inline const char* SetListFileName() { return "sets.lst"; }
	// The name of the name list.
	inline const char* NameListFileName() { return "names.lst"; }

	// What decides whether a folder's files can be used. A change in any field makes a new
	// folder: a game update (section), a pak change (pakHash), a format change (revision) or a
	// size change (storedSize).
	struct FolderKey
	{
		std::wstring section;   // ScanCache::SectionName: ProductVersion, PE stamp and image size
		uint64_t     pakHash    = 0;
		uint32_t     revision   = kRevision;
		uint32_t     storedSize = 0;
	};

	// A folder name that is valid on Windows and says what is in the key.
	std::wstring FolderName(const FolderKey& key);

	// What a file looked like when the key was taken: its size and last-write time. Both zero
	// for a file that is not there (a pak without .utoc/.ucas).
	struct FileStamp
	{
		uint64_t size      = 0;
		uint64_t writeTime = 0;   // FILETIME ticks
	};

	// One mounted pak, with everything that decides which pixels the game shows from it: where it
	// is and where it is mounted, its read order, and the size and time of the pak file and of the
	// .utoc/.ucas beside it (a pak updated in place under the same name changes them).
	struct PakEntry
	{
		std::string path;
		std::string mountPoint;
		int32_t     order    = 0;
		int32_t     numFiles = 0;
		FileStamp   pak;
		FileStamp   utoc;
		FileStamp   ucas;
	};

	// A hash of the mounted pak set. The order the list comes in does not matter; each pak's own
	// read order is part of the hash, so two paks that swap priority give another value.
	uint64_t HashPakSet(std::vector<PakEntry> paks);

	// Whether a normalised object path names an object the engine makes at run time (the transient
	// package, "/Engine/Transient.Texture2D_12", and in-memory packages under "/Memory/"). Such a
	// name is handed out again for a different texture in the next session, so nothing is saved
	// under it or served for it.
	bool IsTransientPath(const std::string& normalizedPath);
}
