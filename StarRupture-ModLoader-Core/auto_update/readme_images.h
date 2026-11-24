#pragma once

#include "auto_update/mod_index.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// readme_images.h -- which picture addresses a README may use, and which
// downloaded bytes count as a picture. Pure functions, no I/O.
//
// A README comes from a third-party repository, so the rules are narrow:
//
//   * https only, no user info, port, backslash, control or non-ASCII character,
//     at most kMaxUrlLen (200) bytes (the shared check in mod_index.h);
//   * hosts: raw.githubusercontent.com and user-images.githubusercontent.com
//     (any path), and github.com with a path of /<owner>/<repo>/raw/... or
//     /user-attachments/...;
//   * never .svg or .svgz (ignoring a query string or fragment).
//
// A relative path in the README is resolved against the directory of the
// entry's readme_url, then goes through the same check.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	enum class ImageFormat { None, Png, Jpeg, Gif, WebP, Bmp };

	namespace detail
	{
		// The non-empty pieces of a path, split on '/'.
		inline std::vector<std::string> SplitPath(const std::string& path)
		{
			std::vector<std::string> parts;
			size_t i = 0;
			while (i <= path.size())
			{
				size_t slash = path.find('/', i);
				if (slash == std::string::npos)
					slash = path.size();
				if (slash > i)
					parts.push_back(path.substr(i, slash - i));
				i = slash + 1;
			}
			return parts;
		}

		inline bool EndsWithNoCase(const std::string& s, const char* suffix)
		{
			size_t n = 0;
			while (suffix[n])
				++n;
			if (s.size() < n)
				return false;
			for (size_t i = 0; i < n; ++i)
				if (Lower(s[s.size() - n + i]) != suffix[i])
					return false;
			return true;
		}
	}

	// See the rules above.
	inline bool IsAllowedImageUrl(const std::string& url)
	{
		static const char* const kHosts[] = { "raw.githubusercontent.com", "user-images.githubusercontent.com", "github.com" };
		if (!detail::IsAllowedUrl(url, kHosts))
			return false;

		constexpr size_t kHostStart = 8;   // after "https://"
		const size_t hostEnd = url.find_first_of("/?#", kHostStart);
		if (hostEnd == std::string::npos || url[hostEnd] != '/')
			return false;                  // no path
		const size_t pathEnd = url.find_first_of("?#", hostEnd);
		const std::string path = url.substr(hostEnd, pathEnd == std::string::npos ? std::string::npos : pathEnd - hostEnd);

		if (detail::EndsWithNoCase(path, ".svg") || detail::EndsWithNoCase(path, ".svgz"))
			return false;

		if (!detail::EqualsNoCase(url.substr(kHostStart, hostEnd - kHostStart), "github.com"))
			return path.size() > 1;

		const std::vector<std::string> seg = detail::SplitPath(path);
		return (seg.size() >= 4 && seg[2] == "raw") || (seg.size() >= 2 && seg[0] == "user-attachments");
	}

	// The address of an image named in a README. An absolute address comes back
	// as it is (the caller still applies IsAllowedImageUrl). A relative one is
	// resolved against the directory of `readmeUrl`
	// (https://raw.githubusercontent.com/<owner>/<repo>/<ref>/<dir>/<file>):
	// "./" and "../" are resolved but never climb above <owner>/<repo>/<ref>
	// (extra "../" stop at that root), and a leading "/" means that root. A
	// fragment is dropped, a query string kept. Empty when it cannot be
	// resolved (a "//host" address, a base that is not a raw README address,
	// or a path that names no file).
	inline std::string ResolveImageUrl(const std::string& src, const std::string& readmeUrl)
	{
		const size_t firstSpecial = src.find_first_of(":/?#");
		if (firstSpecial != std::string::npos && src[firstSpecial] == ':')
			return src;
		if (src.compare(0, 2, "//") == 0)
			return {};

		static const char kBase[] = "https://raw.githubusercontent.com/";
		constexpr size_t kBaseLen = sizeof(kBase) - 1;
		if (readmeUrl.compare(0, kBaseLen, kBase) != 0)
			return {};
		const size_t baseEnd = readmeUrl.find_first_of("?#", kBaseLen);
		const std::vector<std::string> base = detail::SplitPath(readmeUrl.substr(kBaseLen, baseEnd == std::string::npos ? std::string::npos : baseEnd - kBaseLen));
		if (base.size() < 4)
			return {};                     // owner, repo, ref and a file name

		const size_t pathEnd = src.find_first_of("?#");
		const std::string path = src.substr(0, pathEnd);
		std::string query;
		if (pathEnd != std::string::npos && src[pathEnd] == '?')
		{
			const size_t frag = src.find('#', pathEnd);
			query = src.substr(pathEnd, frag == std::string::npos ? std::string::npos : frag - pathEnd);
		}

		std::vector<std::string> stack;    // below the root
		if (path.empty() || path[0] != '/')
			stack.assign(base.begin() + 3, base.end() - 1);
		for (const std::string& part : detail::SplitPath(path))
		{
			if (part == ".")
				continue;
			if (part == "..")
			{
				if (!stack.empty())
					stack.pop_back();
				continue;
			}
			stack.push_back(part);
		}
		if (stack.empty())
			return {};

		std::string out = std::string(kBase) + base[0] + "/" + base[1] + "/" + base[2];
		for (const std::string& part : stack)
			out += "/" + part;
		return out + query;
	}

	// The picture format the bytes start with, by signature only. None for
	// anything else, markup included.
	inline ImageFormat DetectImageFormat(const std::string& b)
	{
		auto at = [&](size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; };
		if (b.size() >= 8 && b.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0)
			return ImageFormat::Png;
		if (b.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF)
			return ImageFormat::Jpeg;
		if (b.size() >= 6 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0))
			return ImageFormat::Gif;
		if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0)
			return ImageFormat::WebP;
		if (b.size() >= 26 && b[0] == 'B' && b[1] == 'M')
		{
			const unsigned headerSize = at(14) | (at(15) << 8) | (at(16) << 16) | (at(17) << 24);
			if (headerSize == 12 || headerSize == 40 || headerSize == 52 || headerSize == 56 || headerSize == 108 || headerSize == 124)
				return ImageFormat::Bmp;
		}
		return ImageFormat::None;
	}

	constexpr int     kMaxImageSide   = 4096;       // a picture wider or taller is refused
	constexpr int64_t kMaxImagePixels = 16000000;   // so is one with more pixels than this

	namespace detail
	{
		inline unsigned Byte(const std::string& b, size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; }
		inline unsigned Le16(const std::string& b, size_t i) { return Byte(b, i) | (Byte(b, i + 1) << 8); }
		inline unsigned Be16(const std::string& b, size_t i) { return (Byte(b, i) << 8) | Byte(b, i + 1); }
		inline uint32_t Le32(const std::string& b, size_t i) { return Le16(b, i) | (static_cast<uint32_t>(Le16(b, i + 2)) << 16); }
		inline uint32_t Be32(const std::string& b, size_t i) { return (static_cast<uint32_t>(Be16(b, i)) << 16) | Be16(b, i + 2); }
	}

	// The pixel size written in the header of a PNG, GIF, BMP, JPEG or WebP (VP8,
	// VP8L, VP8X), read without decoding anything. Every read is bounds-checked.
	// False when the bytes are cut short, the header is not what the format says,
	// or a side is zero. The sizes are as stored, so a side can be huge.
	inline bool ReadImageSize(const std::string& b, int64_t* width, int64_t* height)
	{
		using namespace detail;
		int64_t w = 0;
		int64_t h = 0;
		switch (DetectImageFormat(b))
		{
		case ImageFormat::Png:
			// 8 byte signature, then the IHDR chunk: length, "IHDR", width, height.
			if (b.size() < 24 || b.compare(12, 4, "IHDR") != 0)
				return false;
			w = Be32(b, 16);
			h = Be32(b, 20);
			break;

		case ImageFormat::Gif:
			if (b.size() < 10)
				return false;
			w = Le16(b, 6);
			h = Le16(b, 8);
			break;

		case ImageFormat::Bmp:
			// DetectImageFormat has checked 26 bytes and the DIB header size.
			if (Le32(b, 14) == 12)
			{
				w = Le16(b, 18);
				h = Le16(b, 20);
			}
			else
			{
				w = static_cast<int32_t>(Le32(b, 18));
				h = static_cast<int32_t>(Le32(b, 22));
				if (w < 0)
					w = -w;
				if (h < 0)
					h = -h;   // negative height means top-down rows
			}
			break;

		case ImageFormat::Jpeg:
		{
			size_t i = 2;
			for (;;)
			{
				// Each marker is 0xFF, then its code; 0xFF bytes in between are padding.
				while (i < b.size() && Byte(b, i) == 0xFF && Byte(b, i + 1) == 0xFF)
					++i;
				if (i + 2 > b.size() || Byte(b, i) != 0xFF)
					return false;
				const unsigned code = Byte(b, i + 1);
				i += 2;
				if (code == 0x01 || (code >= 0xD0 && code <= 0xD8))
					continue;                       // no length
				if (code == 0xD9 || code == 0xDA || code == 0x00)
					return false;                   // end of image, scan data or not a marker: no frame header seen
				if (i + 2 > b.size())
					return false;
				const size_t length = Be16(b, i);
				const bool frame = code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 && code != 0xCC;
				if (frame)
				{
					// length, precision, height, width
					if (length < 8 || i + 7 > b.size())
						return false;
					h = Be16(b, i + 3);
					w = Be16(b, i + 5);
					break;
				}
				if (length < 2)
					return false;
				i += length;
			}
			break;
		}

		case ImageFormat::WebP:
		{
			// "RIFF", size, "WEBP", then the first chunk: tag, size, data.
			if (b.size() < 21)
				return false;
			if (b.compare(12, 4, "VP8 ") == 0)
			{
				// 3 byte frame tag, start code 9D 01 2A, 14 bit width and height.
				if (b.size() < 30 || Byte(b, 23) != 0x9D || Byte(b, 24) != 0x01 || Byte(b, 25) != 0x2A)
					return false;
				w = Le16(b, 26) & 0x3FFF;
				h = Le16(b, 28) & 0x3FFF;
			}
			else if (b.compare(12, 4, "VP8L") == 0)
			{
				// signature 0x2F, then 14 bit (width - 1) and 14 bit (height - 1).
				if (b.size() < 25 || Byte(b, 20) != 0x2F)
					return false;
				const uint32_t bits = Le32(b, 21);
				w = static_cast<int64_t>(bits & 0x3FFF) + 1;
				h = static_cast<int64_t>((bits >> 14) & 0x3FFF) + 1;
			}
			else if (b.compare(12, 4, "VP8X") == 0)
			{
				// flags, 3 reserved bytes, then 24 bit (width - 1) and (height - 1).
				if (b.size() < 30)
					return false;
				w = static_cast<int64_t>(Le16(b, 24) | (Byte(b, 26) << 16)) + 1;
				h = static_cast<int64_t>(Le16(b, 27) | (Byte(b, 29) << 16)) + 1;
			}
			else
			{
				return false;
			}
			break;
		}

		default:
			return false;
		}

		if (w <= 0 || h <= 0)
			return false;
		*width = w;
		*height = h;
		return true;
	}

	// Whether the bytes may be handed to the decoder: the size can be read, no side
	// is over kMaxImageSide and the pixel count is within kMaxImagePixels. Decoders
	// allocate the whole decoded picture first, so this runs before they do.
	inline bool IsImageSizeAllowed(const std::string& b)
	{
		int64_t w = 0;
		int64_t h = 0;
		if (!ReadImageSize(b, &w, &h))
			return false;
		return w <= kMaxImageSide && h <= kMaxImageSide && w * h <= kMaxImagePixels;
	}

	// True when the bytes read as SVG, XML or HTML: after an optional byte
	// order mark and white space they start with <svg, <?xml, <!doctype or <html.
	inline bool LooksLikeMarkup(const std::string& b)
	{
		size_t i = 0;
		if (b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0)
			i = 3;
		while (i < b.size() && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
			++i;
		static const char* const kStarts[] = { "<svg", "<?xml", "<!doctype", "<html" };
		for (const char* start : kStarts)
		{
			size_t n = 0;
			while (start[n])
				++n;
			if (b.size() - i < n)
				continue;
			bool same = true;
			for (size_t k = 0; k < n && same; ++k)
				same = detail::Lower(b[i + k]) == start[k];
			if (same)
				return true;
		}
		return false;
	}
}
