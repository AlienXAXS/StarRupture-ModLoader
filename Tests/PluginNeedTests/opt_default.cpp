// Optional unset: it is the need, so a plugin that sets only a need (here the default 75) has no accessors above it.
#include "opt_checks.h"
static_assert(PLUGIN_OPTIONAL_INTERFACE == PLUGIN_NEEDS_INTERFACE, "optional defaults to the need");
