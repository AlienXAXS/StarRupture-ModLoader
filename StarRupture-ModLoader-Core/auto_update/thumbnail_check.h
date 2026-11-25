#pragma once

#include "auto_update/mod_index.h"

#include <cstddef>
#include <string>

// ---------------------------------------------------------------------------
// thumbnail_check.h -- what a thumbnail's bytes must look like before anything
// decodes them. Header only, so a test can use it without the loader.
//
// Only a PNG passes: the 8-byte signature, then the IHDR chunk (length 13),
// whose width and height are read here, so a picture too big to draw is
// refused without a decoder ever seeing it.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	enum class ThumbnailCheck
	{
		Ok,
		NotPng,      // wrong signature, no IHDR first, or cut off
		BadSize,     // a width or height of 0, or over kMaxThumbnailWidth x kMaxThumbnailHeight
	};

	// Checks the header of `bytes`. width and height (when given) are set when
	// the header could be read, even if the size is then refused.
	inline ThumbnailCheck CheckThumbnailHeader(const std::string& bytes, unsigned* width = nullptr, unsigned* height = nullptr)
	{
		static const char kSignature[8] = { '\x89', 'P', 'N', 'G', '\r', '\n', '\x1A', '\n' };
		constexpr size_t kIhdrAt = 8;      // chunk length (4), type (4), then the data
		constexpr size_t kHeaderLen = kIhdrAt + 8 + 8;   // ... width (4), height (4)
		if (bytes.size() < kHeaderLen || bytes.compare(0, sizeof(kSignature), kSignature, sizeof(kSignature)) != 0)
			return ThumbnailCheck::NotPng;

		const auto be32 = [&](size_t at)
		{
			unsigned v = 0;
			for (size_t i = 0; i < 4; ++i)
				v = (v << 8) | static_cast<unsigned char>(bytes[at + i]);
			return v;
		};
		if (be32(kIhdrAt) != 13 || bytes.compare(kIhdrAt + 4, 4, "IHDR") != 0)
			return ThumbnailCheck::NotPng;

		const unsigned w = be32(kIhdrAt + 8);
		const unsigned h = be32(kIhdrAt + 12);
		if (width)
			*width = w;
		if (height)
			*height = h;
		return (w < 1 || h < 1 || w > kMaxThumbnailWidth || h > kMaxThumbnailHeight) ? ThumbnailCheck::BadSize : ThumbnailCheck::Ok;
	}
}
