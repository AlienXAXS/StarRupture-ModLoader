#pragma once

// What the logs may say about the process command line: the program's file
// name and the switch names, never a value. A dedicated server's command line
// can carry a password (-RCONPassword=..., or Map?Password=... in the URL), and
// a log is something people attach to bug reports.
//
// The line is split the way Windows splits it (double quotes group a token,
// a token may hold spaces). The first token is the program; of the rest, a
// token that starts with "-" is kept up to its first "=" and every other token
// is dropped. That covers -Key=Value, -Key Value and /Map?Password=x.
//
// Known limit: in the "-Key Value" form a value that itself starts with "-"
// is logged as a switch name.
//
// Header-only: the loader and the proxy both use it.

#include <string>
#include <vector>

namespace CommandLineLog
{
	// Split like CommandLineToArgvW: the program name ends at the next quote or
	// space with no escapes; after it, 2n backslashes before a quote give n
	// backslashes and a quote that opens or closes a group, 2n+1 give n
	// backslashes and a literal quote.
	inline std::vector<std::wstring> Split(const wchar_t* line)
	{
		std::vector<std::wstring> tokens;
		if (!line)
			return tokens;
		const wchar_t* p = line;

		while (*p == L' ' || *p == L'\t') ++p;
		if (!*p)
			return tokens;

		std::wstring program;
		if (*p == L'"')
		{
			++p;
			while (*p && *p != L'"') program += *p++;
			if (*p == L'"') ++p;
		}
		else
		{
			while (*p && *p != L' ' && *p != L'\t') program += *p++;
		}
		tokens.push_back(program);

		for (;;)
		{
			while (*p == L' ' || *p == L'\t') ++p;
			if (!*p)
				break;

			std::wstring token;
			bool quoted = false;
			while (*p && (quoted || (*p != L' ' && *p != L'\t')))
			{
				size_t backslashes = 0;
				while (*p == L'\\') { ++backslashes; ++p; }
				if (*p == L'"')
				{
					token.append(backslashes / 2, L'\\');
					if (backslashes % 2)
						token += L'"';
					else
						quoted = !quoted;
					++p;
				}
				else
				{
					token.append(backslashes, L'\\');
					if (*p)
						token += *p++;
				}
			}
			tokens.push_back(token);
		}
		return tokens;
	}

	// "Game.exe -log -RCONPassword", for a log line.
	inline std::wstring Redact(const wchar_t* line)
	{
		const std::vector<std::wstring> tokens = Split(line);
		if (tokens.empty())
			return std::wstring();

		const std::wstring& program = tokens[0];
		const size_t slash = program.find_last_of(L"\\/");
		std::wstring out = slash == std::wstring::npos ? program : program.substr(slash + 1);

		for (size_t i = 1; i < tokens.size(); ++i)
		{
			if (tokens[i].empty() || tokens[i][0] != L'-')
				continue;
			out += L' ';
			out += tokens[i].substr(0, tokens[i].find(L'='));
		}
		return out;
	}
}
