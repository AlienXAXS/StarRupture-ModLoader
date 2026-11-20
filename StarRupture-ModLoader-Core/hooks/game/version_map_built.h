#pragma once

// The built-in version map (version_map.generated.h, written by CI) holds one
// kBuiltCL: the game build the Client's SDK was made for. The Client and the
// Server are compiled against different SDK snapshots, so VersionMap::kBuiltCL
// is the CL of the SDK this configuration is built against. It is read from the
// build_cl.txt beside that SDK (MODLOADER_BUILT_CL, set in Shared.props), so it
// moves with the SDK pin. Without the marker it is the generated map's value.
//
// Include this, not version_map.generated.h, wherever kBuiltCL is read.

#define kBuiltCL kMapBuiltCL
#include "hooks/game/version_map.generated.h"
#undef kBuiltCL

namespace VersionMap
{
#if defined(MODLOADER_BUILT_CL)
	inline constexpr uint32_t kBuiltCL = MODLOADER_BUILT_CL;
#else
	inline constexpr uint32_t kBuiltCL = kMapBuiltCL;
#endif

#if defined(MODLOADER_CLIENT_BUILD) && defined(MODLOADER_BUILT_CL)
	// The generated map's CL is what CI reads as the build the loader was written
	// for, so on the Client it moves with the SDK pin.
	static_assert(kBuiltCL == kMapBuiltCL, "kBuiltCL in version_map.generated.h must be the CL of the Client SDK (Client\\build_cl.txt)");
#endif
}
