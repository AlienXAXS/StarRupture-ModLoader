// sr-aobscan --map: reads a version map from a generated header.
//
// The header is read as the data it is: the kBuilds, kFunctionRows,
// kFieldRows, kFunctions and kFields arrays, one brace-enclosed record per
// line, each field a number, true, false, nullptr or a quoted string. The
// kFunctions and kFields arrays give the key of each run of rows.

#include "map_file.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
	using Record = std::vector<std::string>;   // a nullptr is the empty string

	bool Read(const std::wstring& path, std::string& text)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		std::ostringstream buffer;
		buffer << in.rdbuf();
		text = buffer.str();
		return true;
	}

	// The fields of a record line such as { 1, 0x2, "c, d", nullptr }, or false for a line that is not one.
	bool SplitRecord(const std::string& line, Record& out)
	{
		const size_t open = line.find('{');
		const size_t close = line.rfind('}');
		if (open == std::string::npos || close == std::string::npos || close < open)
			return false;
		out.clear();
		std::string field;
		bool quoted = false;
		for (size_t i = open + 1; i < close; ++i)
		{
			const char c = line[i];
			if (quoted)
			{
				if (c == '\\' && i + 1 < close)
					field += line[++i];
				else if (c == '"')
					quoted = false;
				else
					field += c;
			}
			else if (c == '"')
				quoted = true;
			else if (c == ',')
			{
				out.push_back(field);
				field.clear();
			}
			else if (c != ' ' && c != '\t')
				field += c;
		}
		out.push_back(field);
		for (std::string& f : out)
			if (f == "nullptr")
				f.clear();
		return !quoted;
	}

	// The records of the array named `name`, between its "= {{" and "}};".
	bool ArrayRecords(const std::string& text, const char* name, std::vector<Record>& out, std::string& error)
	{
		const size_t at = text.find(std::string(" ") + name + " = {{");
		const size_t end = at == std::string::npos ? at : text.find("}};", at);
		if (at == std::string::npos || end == std::string::npos)
		{
			error = std::string("no array ") + name;
			return false;
		}
		std::istringstream lines(text.substr(at, end - at));
		std::string line;
		std::getline(lines, line);   // the declaration
		while (std::getline(lines, line))
		{
			Record r;
			if (line.find('{') != std::string::npos && SplitRecord(line, r))
				out.push_back(std::move(r));
		}
		return true;
	}

	int64_t Num(const std::string& s) { return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 0)); }

	bool ReadHeader(const std::string& text, VersionMap::MapData& out, std::string& error)
	{
		std::vector<Record> builds, functionRows, fieldRows, functionIndex, fieldIndex;
		if (!ArrayRecords(text, "kBuilds", builds, error) || !ArrayRecords(text, "kFunctionRows", functionRows, error)
			|| !ArrayRecords(text, "kFieldRows", fieldRows, error) || !ArrayRecords(text, "kFunctions", functionIndex, error)
			|| !ArrayRecords(text, "kFields", fieldIndex, error))
			return false;
		VersionMap::MapData m;
		for (const Record& r : builds)
			if (r.size() == 4)
				m.builds.push_back({ static_cast<uint32_t>(Num(r[0])), static_cast<uint32_t>(Num(r[1])), static_cast<uint32_t>(Num(r[2])), r[3] });
		m.functions.resize(functionRows.size());
		for (const Record& r : functionIndex)
		{
			if (r.size() != 3 || Num(r[1]) + Num(r[2]) > static_cast<int64_t>(functionRows.size()))
			{
				error = "kFunctions does not fit kFunctionRows";
				return false;
			}
			for (int64_t i = Num(r[1]); i < Num(r[1]) + Num(r[2]); ++i)
			{
				const Record& f = functionRows[static_cast<size_t>(i)];
				if (f.size() != 12)
				{
					error = "a kFunctionRows record is not 12 fields";
					return false;
				}
				m.functions[static_cast<size_t>(i)] = { r[0], static_cast<uint32_t>(Num(f[0])), static_cast<uint32_t>(Num(f[1])),
					static_cast<uint32_t>(Num(f[2])), static_cast<int32_t>(Num(f[3])), static_cast<uint32_t>(Num(f[4])), f[5] == "true",
					f[6], static_cast<int32_t>(Num(f[7])), static_cast<int32_t>(Num(f[8])), static_cast<int32_t>(Num(f[9])),
					static_cast<uint32_t>(Num(f[10])), f[11] };
			}
		}
		m.fields.resize(fieldRows.size());
		for (const Record& r : fieldIndex)
		{
			if (r.size() != 3 || Num(r[1]) + Num(r[2]) > static_cast<int64_t>(fieldRows.size()))
			{
				error = "kFields does not fit kFieldRows";
				return false;
			}
			for (int64_t i = Num(r[1]); i < Num(r[1]) + Num(r[2]); ++i)
			{
				const Record& f = fieldRows[static_cast<size_t>(i)];
				if (f.size() != 5)
				{
					error = "a kFieldRows record is not 5 fields";
					return false;
				}
				m.fields[static_cast<size_t>(i)] = { r[0], static_cast<uint32_t>(Num(f[0])), static_cast<int32_t>(Num(f[1])),
					static_cast<uint32_t>(Num(f[2])), static_cast<uint8_t>(Num(f[3])), f[4] };
			}
		}
		out = std::move(m);
		return true;
	}
}

namespace AobScan
{
	bool ReadMapFile(const std::wstring& path, VersionMap::MapData& out, std::string& error)
	{
		std::string text;
		if (!Read(path, text))
		{
			error = "cannot read the file";
			return false;
		}
		return ReadHeader(text, out, error);
	}
}
