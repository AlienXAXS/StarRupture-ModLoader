// RailJunctionFixer.cpp : Defines the exported functions for the DLL.
//
#include "RailJunctionFixer.h"
#include "plugin.h"

// Note: The plugin exports (GetPluginInfo, PluginInit, PluginShutdown)
// are now defined in plugin.cpp

// This is an example of an exported variable
RAILJUNCTIONFIXER_API int nRailJunctionFixer=0;

// This is an example of an exported function.
RAILJUNCTIONFIXER_API int fnRailJunctionFixer(void)
{
    return 0;
}

// This is the constructor of a class that has been exported.
CRailJunctionFixer::CRailJunctionFixer()
{
    return;
}
