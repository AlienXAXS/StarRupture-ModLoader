#pragma once

#include "plugin_interface.h"

// The call to a plugin's optional PluginSetHostInfo export. Kept in a header of its
// own so a test can run the same call against a stand-in plugin. Plain data only, no
// objects with destructors, because __try cannot share a function with them (C2712).
namespace PluginHostInfoCall
{
	// Tells the plugin the loader's interface level. Returns false when the export
	// faulted; the filter sees the exception and decides whether to handle it.
	inline bool Run(PluginSetHostInfoFunc fn, LONG (*filter)(EXCEPTION_POINTERS*))
	{
		PluginHostInfo info = { sizeof(PluginHostInfo), PLUGIN_INTERFACE_VERSION_MAX };
		__try { fn(&info); return true; }
		__except (filter(GetExceptionInformation())) { return false; }
	}
}
