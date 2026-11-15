#pragma once

// The size-exact copy of the image library's stored picture: level 0 resampled to the size an
// icon is drawn at, with a Lanczos-3 filter on premultiplied colour so edges neither darken nor
// glow. Plain CPU code with no engine and no D3D, so the rules are testable with bytes; the
// reader's worker calls it.

#include <cstdint>
#include <vector>

namespace ImageResample
{
	// Resamples width x height 8-bit RGBA (straight alpha, tightly packed) to outWidth x outHeight.
	// A size equal to the input comes back unchanged. out is resized to outWidth * outHeight * 4.
	// A zero size leaves out empty.
	void Resample(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight,
		std::vector<uint8_t>& out);
}
