#pragma once
// sr-aobscan --layout: where a bitfield member sits in the byte that holds it.
// See layout.cpp.

#include <cstdint>

namespace AobScan
{
	struct BitfieldPlace
	{
		bool     inOneByte = false;
		uint64_t offset    = 0;  // the byte that holds the field
		uint32_t mask      = 0;  // the field's bits within that byte
	};

	// A bitfield of len bits that starts at bit (counted from the storage unit at
	// memberOffset) and stays inside one byte can be described the way
	// FBoolProperty describes a bool: that byte and a mask. A wider field, or one
	// that crosses a byte boundary, cannot.
	constexpr BitfieldPlace PlaceBitfield(uint64_t memberOffset, uint64_t bit, uint64_t len)
	{
		if (len < 1 || bit % 8 + len > 8)
			return {};
		return { true, memberOffset + bit / 8, ((1u << len) - 1u) << (bit % 8) };
	}
}
