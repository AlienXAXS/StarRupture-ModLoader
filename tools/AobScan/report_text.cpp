// sr-aobscan: the text every report shares: a ScanValidation detail as lines
// under a row, and a string as JSON.

#include "plugin_scan.h"

#include <cstdio>

namespace
{
	// Calls fn on each non-empty line of a ScanValidation detail.
	template <typename Fn>
	void ForEachDetailLine(const std::string& detail, Fn&& fn)
	{
		size_t start = 0;
		while (start <= detail.size())
		{
			const size_t end = detail.find("\r\n", start);
			const std::string line = detail.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty())
				fn(line);
			if (end == std::string::npos)
				break;
			start = end + 2;
		}
	}
}

namespace AobScan
{
	void PrintDetail(FILE* out, const std::string& detail)
	{
		ForEachDetailLine(detail, [out](const std::string& line) { fprintf(out, "    %s\n", line.c_str()); });
	}

	std::string JsonDetail(const std::string& detail)
	{
		std::string out;
		ForEachDetailLine(detail, [&out](const std::string& line) {
			const size_t first = line.find_first_not_of(' ');
			if (first == std::string::npos)
				return;
			if (!out.empty())
				out += '\n';
			out += line.substr(first);
		});
		return out;
	}

	std::string JsonEscape(const std::string& s)
	{
		std::string out;
		for (const unsigned char c : s)
		{
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					sprintf_s(buf, "\\u%04x", c);
					out += buf;
				}
				else
					out += static_cast<char>(c);
			}
		}
		return out;
	}
}
