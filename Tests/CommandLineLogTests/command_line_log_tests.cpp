// CommandLineLogTests: the command line the loader and the proxy write to their
// logs (core/command_line_log.h) holds the program's file name and the switch
// names, and never a value. Every value below is made up.
#include "core/command_line_log.h"

#include <cstdio>
#include <string>

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	bool Has(const std::wstring& text, const wchar_t* part)
	{
		return text.find(part) != std::wstring::npos;
	}

	// The redacted text must be exactly `expected` and must not hold the made-up secret.
	void Case(const wchar_t* line, const wchar_t* expected, const char* what)
	{
		const std::wstring out = CommandLineLog::Redact(line);
		Check(out == expected, what);
		Check(!Has(out, L"secret"), what);
		if (out != expected)
			printf("  got: %ls\n  want: %ls\n", out.c_str(), expected);
	}

	void TestSwitches()
	{
		Case(L"Game.exe -RCONPassword=secret", L"Game.exe -RCONPassword", "-Key=Value");
		Case(L"Game.exe -RCONPassword secret", L"Game.exe -RCONPassword", "-Key Value");
		Case(L"Game.exe -log -RCONPassword=secret -port=7777 -nosplash",
		     L"Game.exe -log -RCONPassword -port -nosplash", "several switches");
		Case(L"Game.exe /Game/Maps/X?listen?Password=secret -log", L"Game.exe -log", "URL argument");
		Case(L"Game.exe -Opt=a=secret", L"Game.exe -Opt", "cut at the first =");
	}

	void TestQuotes()
	{
		Case(L"Game.exe -Key=\"a secret b\"", L"Game.exe -Key", "quoted value after =");
		Case(L"Game.exe -Key \"a secret b\"", L"Game.exe -Key", "quoted value after a space");
		Case(L"Game.exe \"-Key=a secret b\" -log", L"Game.exe -Key -log", "whole token quoted");
		Case(L"Game.exe -Key=\"a \\\"secret\\\" b\" -log", L"Game.exe -Key -log", "escaped quotes in a value");
		Case(L"Game.exe -Key=\"secret\\\\\" -log", L"Game.exe -Key -log", "backslashes before the closing quote");
	}

	void TestProgram()
	{
		Case(L"\"C:\\Program Files\\Some Game\\Game.exe\" -log", L"Game.exe -log", "path with spaces");
		Case(L"C:\\Games\\Game.exe -log", L"Game.exe -log", "path without spaces");
		Case(L"\"C:\\Program Files\\Some Game\\Game.exe\" -Key=\"a secret b\" -log",
		     L"Game.exe -Key -log", "path with spaces and a quoted value");

		const std::wstring out = CommandLineLog::Redact(L"\"C:\\Program Files\\Some Game\\Game.exe\" -log");
		Check(!Has(out, L"Program Files") && !Has(out, L"C:"), "the directory is not logged");
		Check(Has(out, L"Game.exe") && Has(out, L"-log"), "program and switch name are logged");

		Case(L"Game.exe", L"Game.exe", "only the program");
		Case(L"  Game.exe   -log  ", L"Game.exe -log", "extra spaces");
		Case(L"Game.exe\t-log", L"Game.exe -log", "tab between tokens");
	}

	void TestEmpty()
	{
		Case(L"", L"", "empty line");
		Case(L"   ", L"", "only spaces");
		Check(CommandLineLog::Redact(nullptr).empty(), "null line");
	}

	void TestLimit()
	{
		// The documented limit: a value that starts with "-" after a space reads as a switch.
		const std::wstring out = CommandLineLog::Redact(L"Game.exe -Key -value");
		Check(out == L"Game.exe -Key -value", "a value that starts with - is logged as a switch name");
	}
}

int main()
{
	TestSwitches();
	TestQuotes();
	TestProgram();
	TestEmpty();
	TestLimit();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
