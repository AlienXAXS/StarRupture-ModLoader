// The loader's own build: PLUGIN_INTERFACE_HOST_BUILD makes it see every member.
#define PLUGIN_INTERFACE_HOST_BUILD
#include "need_checks.h"

static_assert(PLUGIN_NEEDS_INTERFACE == PLUGIN_INTERFACE_VERSION_MAX, "the host build sees every member");
