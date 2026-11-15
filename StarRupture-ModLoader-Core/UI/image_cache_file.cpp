#include "UI/image_cache_file.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ImageCache
{
	namespace
	{
		struct CrcTable
		{
			uint32_t entries[256];
			CrcTable()
			{
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t value = i;
					for (int bit = 0; bit < 8; ++bit)
						value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
					entries[i] = value;
				}
			}
		};

		uint64_t Fnv1a(uint64_t hash, const void* data, size_t size)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		constexpr uint64_t kFnvBasis = 1469598103934665603ULL;

		void Put32(std::vector<uint8_t>& out, uint32_t value)
		{
			for (int i = 0; i < 4; ++i)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		void Put64(std::vector<uint8_t>& out, uint64_t value)
		{
			for (int i = 0; i < 8; ++i)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		uint32_t Get32(const uint8_t* at)
		{
			return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 |
			       static_cast<uint32_t>(at[2]) << 16 | static_cast<uint32_t>(at[3]) << 24;
		}

		uint64_t Get64(const uint8_t* at)
		{
			return static_cast<uint64_t>(Get32(at)) | static_cast<uint64_t>(Get32(at + 4)) << 32;
		}

		bool ValidSize(uint32_t width, uint32_t height)
		{
			return width > 0 && height > 0 && width <= kMaxSide && height <= kMaxSide;
		}

		bool Valid(const Info& info)
		{
			return ValidSize(info.storedWidth, info.storedHeight) &&
			       info.sourceWidth > 0 && info.sourceHeight > 0 &&
			       info.levels == ImageShrink::LevelCount(info.storedWidth, info.storedHeight) &&
			       info.levels <= kMaxLevels &&
			       !info.path.empty() && info.path.size() <= kMaxPathLength;
		}

		// The u64 after the nine u32 fields, then the crc.
		constexpr size_t kCrcOffset = 44;
	}

	uint32_t Crc32(const void* data, size_t size, uint32_t seed)
	{
		static const CrcTable table;
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint32_t crc = ~seed;
		for (size_t i = 0; i < size; ++i)
			crc = table.entries[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	std::vector<uint8_t> Serialize(const Info& info, const uint8_t* pixels, size_t pixelBytes)
	{
		if (!Valid(info) || !pixels)
			return {};
		const uint64_t expected = ImageShrink::Bytes({ info.storedWidth, info.storedHeight }, info.levels);
		if (pixelBytes != expected)
			return {};

		std::vector<uint8_t> out;
		out.reserve(kHeaderBytes + info.path.size() + pixelBytes);
		Put32(out, kImageMagic);
		Put32(out, kRevision);
		Put32(out, info.sourceFormat);
		Put32(out, info.sourceWidth);
		Put32(out, info.sourceHeight);
		Put32(out, info.storedWidth);
		Put32(out, info.storedHeight);
		Put32(out, info.levels);
		Put32(out, static_cast<uint32_t>(info.path.size()));
		Put64(out, expected);

		uint32_t crc = Crc32(out.data(), out.size());
		crc = Crc32(info.path.data(), info.path.size(), crc);
		crc = Crc32(pixels, pixelBytes, crc);
		Put32(out, crc);

		out.insert(out.end(), info.path.begin(), info.path.end());
		out.insert(out.end(), pixels, pixels + pixelBytes);
		return out;
	}

	Parsed Parse(const uint8_t* data, size_t size, Info& info, const uint8_t*& pixels)
	{
		pixels = nullptr;
		if (!data || size < kHeaderBytes)
			return Parsed::TooShort;
		if (Get32(data) != kImageMagic)
			return Parsed::BadMagic;
		if (Get32(data + 4) != kRevision)
			return Parsed::WrongRevision;

		Info read;
		read.sourceFormat = Get32(data + 8);
		read.sourceWidth  = Get32(data + 12);
		read.sourceHeight = Get32(data + 16);
		read.storedWidth  = Get32(data + 20);
		read.storedHeight = Get32(data + 24);
		read.levels       = Get32(data + 28);
		const uint32_t pathLength   = Get32(data + 32);
		const uint64_t payloadBytes = Get64(data + 36);
		const uint32_t crc          = Get32(data + kCrcOffset);

		if (!ValidSize(read.storedWidth, read.storedHeight) || read.sourceWidth == 0 || read.sourceHeight == 0 ||
			read.levels == 0 || read.levels > kMaxLevels ||
			read.levels != ImageShrink::LevelCount(read.storedWidth, read.storedHeight) ||
			pathLength == 0 || pathLength > kMaxPathLength ||
			payloadBytes != ImageShrink::Bytes({ read.storedWidth, read.storedHeight }, read.levels))
			return Parsed::BadHeader;

		if (size != kHeaderBytes + static_cast<uint64_t>(pathLength) + payloadBytes)
			return Parsed::Truncated;

		const uint8_t* const path = data + kHeaderBytes;
		const uint8_t* const payload = path + pathLength;
		uint32_t check = Crc32(data, kCrcOffset);
		check = Crc32(path, pathLength, check);
		check = Crc32(payload, static_cast<size_t>(payloadBytes), check);
		if (check != crc)
			return Parsed::BadCrc;

		read.path.assign(reinterpret_cast<const char*>(path), pathLength);
		info   = std::move(read);
		pixels = payload;
		return Parsed::Ok;
	}

	const char* Describe(Parsed result)
	{
		switch (result)
		{
		case Parsed::Ok:            return "ok";
		case Parsed::TooShort:      return "too short";
		case Parsed::BadMagic:      return "bad magic";
		case Parsed::WrongRevision: return "wrong revision";
		case Parsed::BadHeader:     return "bad header";
		case Parsed::Truncated:     return "truncated";
		case Parsed::BadCrc:        return "bad crc";
		}
		return "?";
	}

	namespace
	{
		bool FitsList(const std::string& text)
		{
			return !text.empty() && text.size() <= kMaxPathLength;
		}
	}

	// What does not fit the limits ParseSets and ParseNames enforce is left out, so one bad entry
	// cannot make a whole list unreadable.
	std::vector<uint8_t> SerializeSets(const SetList& sets)
	{
		SetList kept;
		for (const SetEntry& entry : sets)
		{
			if (kept.size() >= kMaxSets)
				break;
			SetEntry fit;
			fit.set = entry.set;
			for (const std::string& path : entry.paths)
				if (FitsList(path) && fit.paths.size() < kMaxSetPaths)
					fit.paths.push_back(path);
			kept.push_back(std::move(fit));
		}

		std::vector<uint8_t> out;
		Put32(out, kSetListMagic);
		Put32(out, kRevision);
		Put32(out, static_cast<uint32_t>(kept.size()));
		for (const SetEntry& entry : kept)
		{
			Put32(out, entry.set);
			Put32(out, static_cast<uint32_t>(entry.paths.size()));
			for (const std::string& path : entry.paths)
			{
				Put32(out, static_cast<uint32_t>(path.size()));
				out.insert(out.end(), path.begin(), path.end());
			}
		}
		Put32(out, Crc32(out.data(), out.size()));
		return out;
	}

	Parsed ParseSets(const uint8_t* data, size_t size, SetList& sets)
	{
		if (!data || size < 16)
			return Parsed::TooShort;
		if (Get32(data) != kSetListMagic)
			return Parsed::BadMagic;
		if (Get32(data + 4) != kRevision)
			return Parsed::WrongRevision;
		if (Crc32(data, size - 4) != Get32(data + size - 4))
			return Parsed::BadCrc;

		// The crc holds, so the lengths below can only be wrong if the writer was; they are still
		// checked against the end of the data.
		const size_t end = size - 4;
		size_t at = 8;
		const auto take = [&](uint32_t& value)
		{
			if (end - at < 4)
				return false;
			value = Get32(data + at);
			at += 4;
			return true;
		};

		uint32_t setCount = 0;
		if (!take(setCount) || setCount > kMaxSets)
			return Parsed::BadHeader;

		SetList read;
		for (uint32_t s = 0; s < setCount; ++s)
		{
			SetEntry entry;
			uint32_t pathCount = 0;
			if (!take(entry.set) || !take(pathCount) || pathCount > kMaxSetPaths)
				return Parsed::BadHeader;
			for (uint32_t p = 0; p < pathCount; ++p)
			{
				uint32_t length = 0;
				if (!take(length) || length == 0 || length > kMaxPathLength || end - at < length)
					return Parsed::Truncated;
				entry.paths.emplace_back(reinterpret_cast<const char*>(data + at), length);
				at += length;
			}
			read.push_back(std::move(entry));
		}
		if (at != end)
			return Parsed::Truncated;

		sets = std::move(read);
		return Parsed::Ok;
	}

	std::vector<uint8_t> SerializeNames(const NameList& names)
	{
		NameList kept;
		for (const NamePair& pair : names)
			if (kept.size() < kMaxNames && FitsList(pair.name) && FitsList(pair.path))
				kept.push_back(pair);

		std::vector<uint8_t> out;
		Put32(out, kNameListMagic);
		Put32(out, kRevision);
		Put32(out, static_cast<uint32_t>(kept.size()));
		for (const NamePair& pair : kept)
		{
			Put32(out, static_cast<uint32_t>(pair.name.size()));
			out.insert(out.end(), pair.name.begin(), pair.name.end());
			Put32(out, static_cast<uint32_t>(pair.path.size()));
			out.insert(out.end(), pair.path.begin(), pair.path.end());
		}
		Put32(out, Crc32(out.data(), out.size()));
		return out;
	}

	Parsed ParseNames(const uint8_t* data, size_t size, NameList& names)
	{
		if (!data || size < 16)
			return Parsed::TooShort;
		if (Get32(data) != kNameListMagic)
			return Parsed::BadMagic;
		if (Get32(data + 4) != kRevision)
			return Parsed::WrongRevision;
		if (Crc32(data, size - 4) != Get32(data + size - 4))
			return Parsed::BadCrc;

		const size_t end = size - 4;
		size_t at = 8;
		const auto take = [&](uint32_t& value)
		{
			if (end - at < 4)
				return false;
			value = Get32(data + at);
			at += 4;
			return true;
		};
		const auto takeText = [&](std::string& text)
		{
			uint32_t length = 0;
			if (!take(length) || length == 0 || length > kMaxPathLength || end - at < length)
				return false;
			text.assign(reinterpret_cast<const char*>(data + at), length);
			at += length;
			return true;
		};

		uint32_t count = 0;
		if (!take(count) || count > kMaxNames)
			return Parsed::BadHeader;

		NameList read;
		read.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			NamePair pair;
			if (!takeText(pair.name) || !takeText(pair.path))
				return Parsed::Truncated;
			read.push_back(std::move(pair));
		}
		if (at != end)
			return Parsed::Truncated;

		names = std::move(read);
		return Parsed::Ok;
	}

	std::string FileName(const std::string& normalizedPath)
	{
		char name[32];
		snprintf(name, sizeof(name), "%016llx.img",
			static_cast<unsigned long long>(Fnv1a(kFnvBasis, normalizedPath.data(), normalizedPath.size())));
		return name;
	}

	std::wstring FolderName(const FolderKey& key)
	{
		std::wstring name = key.section;
		for (wchar_t& c : name)
			if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
				c = L'_';

		wchar_t suffix[64]{};
		swprintf_s(suffix, L"_p%016llx_r%u_s%u", static_cast<unsigned long long>(key.pakHash), key.revision, key.storedSize);
		return name + suffix;
	}

	namespace
	{
		uint64_t HashNumber(uint64_t hash, uint64_t value)
		{
			uint8_t bytes[8];
			for (int i = 0; i < 8; ++i)
				bytes[i] = static_cast<uint8_t>(value >> (8 * i));
			return Fnv1a(hash, bytes, sizeof(bytes));
		}

		uint64_t HashStamp(uint64_t hash, const FileStamp& stamp)
		{
			return HashNumber(HashNumber(hash, stamp.size), stamp.writeTime);
		}
	}

	uint64_t HashPakSet(std::vector<PakEntry> paks)
	{
		std::sort(paks.begin(), paks.end(), [](const PakEntry& a, const PakEntry& b)
		{
			if (a.path != b.path)
				return a.path < b.path;
			if (a.mountPoint != b.mountPoint)
				return a.mountPoint < b.mountPoint;
			return a.order < b.order;
		});

		uint64_t hash = kFnvBasis;
		const uint8_t separator = 0;
		for (const PakEntry& pak : paks)
		{
			hash = Fnv1a(hash, pak.path.data(), pak.path.size());
			hash = Fnv1a(hash, &separator, 1);
			hash = Fnv1a(hash, pak.mountPoint.data(), pak.mountPoint.size());
			hash = Fnv1a(hash, &separator, 1);
			hash = HashNumber(hash, static_cast<uint32_t>(pak.order));
			hash = HashNumber(hash, static_cast<uint32_t>(pak.numFiles));
			hash = HashStamp(hash, pak.pak);
			hash = HashStamp(hash, pak.utoc);
			hash = HashStamp(hash, pak.ucas);
		}
		return hash;
	}

	bool IsTransientPath(const std::string& normalizedPath)
	{
		// The package is what comes before the first '.': "/engine/transient" in
		// "/engine/transient.texture2d_12".
		const std::string package = normalizedPath.substr(0, normalizedPath.find('.'));
		const auto is = [&package](const char* name)
		{
			const size_t n = strlen(name);
			return package.size() >= n && _strnicmp(package.c_str(), name, n) == 0;
		};
		return (package.size() == 17 && is("/engine/transient")) || is("/memory/");
	}
}
