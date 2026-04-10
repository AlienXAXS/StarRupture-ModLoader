#pragma once

// ---------------------------------------------------------------------------
// layout.h — aggregate include for all radar entity scanners
// ---------------------------------------------------------------------------

#include "layout_players.h"
#include "layout_basecores.h"
#include "layout_markers.h"
#include "layout_foundables.h"
#include "layout_enemies.h"
#include "layout_hlod.h"
#include "layout_playermarkers.h"
#include "layout_custompins.h"

namespace Layout
{
	// Call this when the world is about to end (OnBeforeWorldEndPlay).
	// Resets all layout scanner state immediately so the next world starts clean.
	inline void NotifyWorldEndPlay()
	{
		ResetBaseCoreState();
	}
}
