// PluginClassTests: the loader made for CL-130045 (fixture130045\ holds the
// version map with that kBuiltCL) turns the same plugins away, and admits the
// same ones, as the loader made for CL-127004 (VersionMapTests). The table is
// in plugin_class_matrix.h. Built from VersionMapTests.vcxproj with
// /p:BuiltFor130045=true.
#include "test_doubles.h"
#include "plugin_class_matrix.h"

#include "memory_scanner/version_map.h"

#include <cstdio>

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			++g_failed;
	}
}

int main()
{
	std::printf("The loader's built CL is %u\n", VersionMap::kBuiltCL);
	Check(VersionMap::kBuiltCL == 130045, "this program is the loader made for 130045");
	PluginClassMatrix::Run([](bool ok, const char* what) { Check(ok, what); });
	std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
