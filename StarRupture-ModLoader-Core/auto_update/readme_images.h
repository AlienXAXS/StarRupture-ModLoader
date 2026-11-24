#pragma once

#include "auto_update/mod_index.h"

#include <cstddef>
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
