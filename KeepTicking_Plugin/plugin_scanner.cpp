#include "plugin_scanner.h"
#include "plugin_logger.h"

namespace PluginScanner
{
	static IPluginScanner* g_scanner = nullptr;

	void Initialize(IPluginScanner* scanner)
	{
		g_scanner = scanner;

		if (g_scanner)
		{
			PluginLogger::Debug("Scanner interface initialized");
		}
		else
		{
			PluginLogger::Error("Scanner interface is NULL!");
		}
	}

	uintptr_t FindPatternInMainModule(const char* pattern)
	{
		if (!g_scanner || !g_scanner->FindPatternInMainModule)
		{
			PluginLogger::Error("Scanner not initialized or FindPatternInMainModule not available");
			return 0;
		}

		return g_scanner->FindPatternInMainModule(pattern);
	}

	uintptr_t FindPatternInModule(HMODULE module, const char* pattern)
	{
		if (!g_scanner || !g_scanner->FindPatternInModule)
		{
			PluginLogger::Error("Scanner not initialized or FindPatternInModule not available");
			return 0;
		}

		return g_scanner->FindPatternInModule(module, pattern);
	}

	std::vector<uintptr_t> FindAllPatternsInMainModule(const char* pattern)
	{
		if (!g_scanner || !g_scanner->FindAllPatternsInMainModule)
		{
			PluginLogger::Error("Scanner not initialized or FindAllPatternsInMainModule not available");
			return {};
		}

		return g_scanner->FindAllPatternsInMainModule(pattern);
	}

	std::vector<uintptr_t> FindAllPatternsInModule(HMODULE module, const char* pattern)
	{
		if (!g_scanner || !g_scanner->FindAllPatternsInModule)
		{
			PluginLogger::Error("Scanner not initialized or FindAllPatternsInModule not available");
			return {};
		}

		return g_scanner->FindAllPatternsInModule(module, pattern);
	}

	uintptr_t FindUniquePattern(const char** patterns, int patternCount, int* outPatternIndex)
	{
		if (!g_scanner || !g_scanner->FindUniquePattern)
		{
			PluginLogger::Error("Scanner not initialized or FindUniquePattern not available");
			return 0;
		}

		return g_scanner->FindUniquePattern(patterns, patternCount, outPatternIndex);
	}
}
