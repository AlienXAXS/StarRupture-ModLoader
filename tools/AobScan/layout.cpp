// sr-aobscan <game.exe> --layout <keys.txt>: the offset, size and bool mask of
// each key, read from the class records in the PDB that ships next to the exe.
//
// A key is "Class::Member" or "Class" (the class's size), in Dumper-7 naming,
// one per line; blank lines and lines starting with '#' are skipped. A member
// is looked up in the class first and then in its base classes, with the base
// offsets added, so an inherited member resolves through the derived class.
// A bitfield bool is reported the way FBoolProperty describes it: the byte that
// holds the bit, size 1, and the bit's mask within that byte. A plain C++ bool
// has mask 0xFF, as its FBoolProperty does.
//
// Where reflection names a thing differently from C++, the C++ name is tried
// too, in this order:
//   * A member: the exact name; then ignoring case, as FName compares
//     (UniqueID / UniqueId); then with "_DEPRECATED" appended, which the
//     reflected name of a deprecated property drops; then, for a name ending in
//     "_<n>", without that suffix and in the same case, which Dumper-7 adds to a
//     member whose name collides with another identifier (Outer_0 for Outer).
//     A plugin's field request (FieldFromPdb) takes only the exact name and the
//     "_DEPRECATED" form, both case-sensitive: the loader's reflection reader
//     compares the member name exactly, so the PDB must not accept a name the
//     game refuses.
//   * A class: the exact name; then the template behind one of the engine's
//     math aliases (FVector is UE::Math::TVector<double>, FVector3f is
//     UE::Math::TVector<float>); then, for
//     Dumper-7's "Owner_Function" parameter struct, the struct UnrealHeaderTool
//     generated for it, "Owner_eventFunction_Parms", splitting at each '_' in
//     turn, since the owner's name may hold one.
//
// Results: OK; NOTYPE (no class of that name with a body); AMBIGUOUS (more than
// one class of that name, laid out differently); NOMEMBER (the class and its
// bases have no data member of that name); BITFIELD (the member is a bitfield
// wider than one bit, which FBoolProperty cannot describe); BADKEY.
//
// Output is the keys as given and numbers only. Type and member names are read
// from the PDB to find the keys and are never printed: the PDB is not ours to
// publish.
#include "layout.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pdb.h"
#include "plugin_scan.h"
#include "memory_scanner/version_map.h"

namespace
{
	// cvconst.h, which ships with DIA rather than with the Windows SDK.
	constexpr DWORD kSymTagData      = 7;
	constexpr DWORD kSymTagUDT       = 11;
	constexpr DWORD kSymTagBaseType  = 16;
	constexpr DWORD kSymTagBaseClass = 18;
	constexpr DWORD kDataIsMember    = 7;
	constexpr DWORD kBtBool          = 10;
	constexpr int   kMaxBaseDepth    = 32;

	// The engine's math aliases (Engine/Source/Runtime/Core/Public/Math/MathFwd.h).
	constexpr struct { const char* alias; const wchar_t* type; } kMathAliases[] = {
		{ "FBox",      L"UE::Math::TBox<double>" },
		{ "FBox2D",    L"UE::Math::TBox2<double>" },
		{ "FMatrix",   L"UE::Math::TMatrix<double>" },
		{ "FPlane",    L"UE::Math::TPlane<double>" },
		{ "FQuat",     L"UE::Math::TQuat<double>" },
		{ "FRotator",  L"UE::Math::TRotator<double>" },
		{ "FSphere",   L"UE::Math::TSphere<double>" },
		{ "FTransform", L"UE::Math::TTransform<double>" },
		{ "FVector",   L"UE::Math::TVector<double>" },
		{ "FVector2D", L"UE::Math::TVector2<double>" },
		{ "FVector4",  L"UE::Math::TVector4<double>" },
		{ "FBox3f",    L"UE::Math::TBox<float>" },
		{ "FBox2f",    L"UE::Math::TBox2<float>" },
		{ "FMatrix44f", L"UE::Math::TMatrix<float>" },
		{ "FPlane4f",  L"UE::Math::TPlane<float>" },
		{ "FQuat4f",   L"UE::Math::TQuat<float>" },
		{ "FRotator3f", L"UE::Math::TRotator<float>" },
		{ "FSphere3f", L"UE::Math::TSphere<float>" },
		{ "FTransform3f", L"UE::Math::TTransform<float>" },
		{ "FVector3f", L"UE::Math::TVector<float>" },
		{ "FVector2f", L"UE::Math::TVector2<float>" },
		{ "FVector4f", L"UE::Math::TVector4<float>" },
		{ "FBox3d",    L"UE::Math::TBox<double>" },
		{ "FBox2d",    L"UE::Math::TBox2<double>" },
		{ "FMatrix44d", L"UE::Math::TMatrix<double>" },
		{ "FPlane4d",  L"UE::Math::TPlane<double>" },
		{ "FQuat4d",   L"UE::Math::TQuat<double>" },
		{ "FRotator3d", L"UE::Math::TRotator<double>" },
		{ "FSphere3d", L"UE::Math::TSphere<double>" },
		{ "FTransform3d", L"UE::Math::TTransform<double>" },
		{ "FVector3d", L"UE::Math::TVector<double>" },
		{ "FVector2d", L"UE::Math::TVector2<double>" },
		{ "FVector4d", L"UE::Math::TVector4<double>" },
	};

	// Any address works: the exe is not mapped, DbgHelp reads it from disk.
	constexpr DWORD64 kModuleBase = 0x140000000;

	struct Key
	{
		std::string text;
		std::string className;
		std::string member;  // empty: the class's size
	};

	struct Answer
	{
		const char* result   = "BADKEY";
		uint64_t    offset   = 0;
		uint64_t    size     = 0;
		uint32_t    boolMask = 0;
	};

	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring out(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
		return out;
	}

	class Types
	{
	public:
		Types(HANDLE session, DWORD64 base) : session_(session), base_(base) {}

		bool Get(DWORD id, IMAGEHLP_SYMBOL_TYPE_INFO what, void* out) const
		{
			return SymGetTypeInfo(session_, base_, id, what, out) != FALSE;
		}

		DWORD Tag(DWORD id) const
		{
			DWORD tag = 0;
			return Get(id, TI_GET_SYMTAG, &tag) ? tag : 0;
		}

		uint64_t Length(DWORD id) const
		{
			ULONG64 length = 0;
			return Get(id, TI_GET_LENGTH, &length) ? length : 0;
		}

		std::wstring Name(DWORD id) const
		{
			WCHAR* name = nullptr;
			if (!Get(id, TI_GET_SYMNAME, &name) || !name)
				return {};
			std::wstring out(name);
			LocalFree(name);
			return out;
		}

		std::vector<DWORD> Children(DWORD id) const
		{
			DWORD count = 0;
			if (!Get(id, TI_GET_CHILDRENCOUNT, &count) || count == 0)
				return {};
			std::vector<uint8_t> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
			auto* params  = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
			params->Count = count;
			params->Start = 0;
			if (!Get(id, TI_FINDCHILDREN, params))
				return {};
			return std::vector<DWORD>(params->ChildId, params->ChildId + count);
		}

		// Every class record named exactly name that has a body. Several records
		// laid out the same (one per translation unit's view) count as one.
		std::vector<DWORD> Classes(const std::wstring& name) const
		{
			struct Context
			{
				const std::wstring* name;
				std::vector<DWORD>  found;
			} ctx{ &name, {} };
			SymEnumTypesByNameW(session_, base_, name.c_str(),
				[](PSYMBOL_INFOW s, ULONG, PVOID user) -> BOOL {
					auto* c = static_cast<Context*>(user);
					if (s->Tag == kSymTagUDT && s->Size != 0 && std::wstring(s->Name, wcsnlen(s->Name, s->NameLen)) == *c->name)
						c->found.push_back(s->TypeIndex);
					return TRUE;
				}, &ctx);
			return Distinct(ctx.found);
		}

		// Every class record named "Outer::name" that has a body, name ending in
		// "_Parms". A wildcard lookup walks the whole type stream, so the first
		// call indexes every nested "_Parms" record in one pass and later calls
		// read the index.
		std::vector<DWORD> NestedParms(const std::wstring& name) const
		{
			if (!parmsIndexed_)
			{
				parmsIndexed_ = true;
				SymEnumTypesByNameW(session_, base_, L"*::*_Parms",
					[](PSYMBOL_INFOW s, ULONG, PVOID user) -> BOOL {
						const std::wstring n(s->Name, wcsnlen(s->Name, s->NameLen));
						const size_t sep = n.rfind(L"::");
						if (s->Tag == kSymTagUDT && s->Size != 0 && sep != std::wstring::npos)
							(*static_cast<std::map<std::wstring, std::vector<DWORD>>*>(user))[n.substr(sep + 2)].push_back(s->TypeIndex);
						return TRUE;
					}, &parms_);
			}
			const auto it = parms_.find(name);
			return it == parms_.end() ? std::vector<DWORD>{} : Distinct(it->second);
		}

		// Finds member in the class or its bases, by the name rules above; offset is
		// from the start of the class.
		bool Member(DWORD udt, const std::wstring& member, Answer& out, bool exact = false) const
		{
			const std::wstring deprecated = member + L"_DEPRECATED";
			if (exact)
				return Find(udt, [&](const std::wstring& n) { return n == member || n == deprecated; }, 0, 0, out);
			if (Find(udt, [&](const std::wstring& n) { return n == member; }, 0, 0, out)
			    || Find(udt, [&](const std::wstring& n) { return _wcsicmp(n.c_str(), member.c_str()) == 0; }, 0, 0, out)
			    || Find(udt, [&](const std::wstring& n) { return _wcsicmp(n.c_str(), deprecated.c_str()) == 0; }, 0, 0, out))
				return true;
			// Dumper-7's suffix is _0, _1, ...: no leading zero, and the base keeps its case.
			const size_t sep = member.rfind(L'_');
			if (sep == std::wstring::npos || sep == 0 || sep + 1 == member.size()
			    || member.find_first_not_of(L"0123456789", sep + 1) != std::wstring::npos
			    || (member[sep + 1] == L'0' && sep + 2 != member.size()))
				return false;
			const std::wstring renamed = member.substr(0, sep);
			return Find(udt, [&](const std::wstring& n) { return n == renamed; }, 0, 0, out);
		}

	private:
		std::vector<DWORD> Distinct(const std::vector<DWORD>& all) const
		{
			std::vector<DWORD> found;
			for (DWORD id : all)
			{
				bool seen = false;
				for (DWORD f : found)
					seen = seen || SameLayout(f, id);
				if (!seen)
					found.push_back(id);
			}
			return found;
		}

		template <typename Match>
		bool Find(DWORD udt, const Match& match, uint64_t baseOffset, int depth, Answer& out) const
		{
			const std::vector<DWORD> children = Children(udt);
			for (DWORD child : children)
			{
				DWORD kind = 0;
				if (Tag(child) != kSymTagData || !Get(child, TI_GET_DATAKIND, &kind) || kind != kDataIsMember
				    || !match(Name(child)))
					continue;
				DWORD offset = 0, type = 0;
				Get(child, TI_GET_OFFSET, &offset);
				Get(child, TI_GET_TYPEID, &type);
				DWORD bit = 0;
				if (Get(child, TI_GET_BITPOSITION, &bit))
				{
					// FBoolProperty describes a one-bit field only.
					if (Length(child) != 1)
					{
						out.result = "BITFIELD";
						return true;
					}
					out.offset   = baseOffset + offset + bit / 8;
					out.size     = 1;
					out.boolMask = 1u << (bit % 8);
				}
				else
				{
					DWORD baseType = 0;
					out.offset   = baseOffset + offset;
					out.size     = Length(type);
					out.boolMask = Tag(type) == kSymTagBaseType && Get(type, TI_GET_BASETYPE, &baseType)
					               && baseType == kBtBool ? 0xFF : 0;
				}
				out.result = "OK";
				return true;
			}
			if (depth >= kMaxBaseDepth)
				return false;
			for (DWORD child : children)
			{
				if (Tag(child) != kSymTagBaseClass)
					continue;
				DWORD offset = 0, type = 0;
				Get(child, TI_GET_OFFSET, &offset);
				Get(child, TI_GET_TYPEID, &type);
				if (Find(type, match, baseOffset + offset, depth + 1, out))
					return true;
			}
			return false;
		}

		// Same size and the same data members at the same offsets.
		bool SameLayout(DWORD a, DWORD b) const
		{
			if (a == b)
				return true;
			if (Length(a) != Length(b))
				return false;
			const std::vector<DWORD> ca = Children(a), cb = Children(b);
			if (ca.size() != cb.size())
				return false;
			for (size_t i = 0; i < ca.size(); ++i)
			{
				DWORD oa = 0, ob = 0;
				Get(ca[i], TI_GET_OFFSET, &oa);
				Get(cb[i], TI_GET_OFFSET, &ob);
				if (Tag(ca[i]) != Tag(cb[i]) || oa != ob || Name(ca[i]) != Name(cb[i]))
					return false;
			}
			return true;
		}

		HANDLE  session_;
		DWORD64 base_;
		mutable bool parmsIndexed_ = false;
		mutable std::map<std::wstring, std::vector<DWORD>> parms_;
	};

	bool ReadKeys(const wchar_t* path, std::vector<Key>& keys)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		std::string line;
		bool first = true;
		while (std::getline(in, line))
		{
			// Windows PowerShell 5.1 writes UTF-8 with a byte-order mark.
			if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
				line.erase(0, 3);
			first = false;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.pop_back();
			size_t start = line.find_first_not_of(" \t");
			if (start == std::string::npos || line[start] == '#')
				continue;
			Key key;
			key.text = line.substr(start);
			const size_t sep = key.text.find("::");
			key.className = key.text.substr(0, sep);
			if (sep != std::string::npos)
				key.member = key.text.substr(sep + 2);
			keys.push_back(std::move(key));
		}
		return true;
	}

	// The class for a Dumper-7 name, or UHT's parameter struct for "Owner_Function".
	std::vector<DWORD> FindClass(const Types& types, const std::string& name)
	{
		std::vector<DWORD> found = types.Classes(Widen(name));
		for (const auto& math : kMathAliases)
			if (found.empty() && name == math.alias)
				found = types.Classes(math.type);
		// The owner's own name may hold a '_', so every split is tried, first to last.
		for (size_t sep = name.find('_'); found.empty() && sep != std::string::npos; sep = name.find('_', sep + 1))
		{
			if (sep == 0 || sep + 1 == name.size())
				continue;
			const std::wstring parms = Widen(name.substr(0, sep) + "_event" + name.substr(sep + 1) + "_Parms");
			// UHT leaves an event's struct at file scope and nests a function's in its Z_Construct_..._Statics.
			found = types.Classes(parms);
			if (found.empty())
				found = types.NestedParms(parms);
		}
		return found;
	}

	// byName: FindClass's answers so far. A lookup by name walks the PDB's type records.
	// exactMember: the member rule of a plugin's field request (above).
	Answer Resolve(const Types& types, const Key& key, std::map<std::string, std::vector<DWORD>>& byName,
		bool exactMember = false)
	{
		Answer answer;
		if (key.className.empty() || (key.text.find("::") != std::string::npos && key.member.empty()))
			return answer;
		auto cached = byName.find(key.className);
		if (cached == byName.end())
			cached = byName.emplace(key.className, FindClass(types, key.className)).first;
		const std::vector<DWORD>& classes = cached->second;
		if (classes.empty())
		{
			answer.result = "NOTYPE";
			return answer;
		}
		if (classes.size() > 1)
		{
			answer.result = "AMBIGUOUS";
			return answer;
		}
		if (key.member.empty())
		{
			answer.result = "OK";
			answer.size   = types.Length(classes[0]);
			return answer;
		}
		if (!types.Member(classes[0], Widen(key.member), answer, exactMember))
			answer.result = "NOMEMBER";
		return answer;
	}
}

namespace AobScan
{
	int ScanLayout(const wchar_t* exePath, const wchar_t* keysPath, bool json)
	{
		std::vector<Key> keys;
		if (!ReadKeys(keysPath, keys) || keys.empty())
		{
			fprintf(stderr, "sr-aobscan: %ls: cannot read the key list, or it has no keys\n", keysPath);
			return kExitBadInput;
		}

		using Clock = std::chrono::steady_clock;
		const auto start  = Clock::now();
		const auto session = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5354));
		const bool loaded  = LoadMatchedPdb(session, exePath, kModuleBase, 0);
		const auto loadedAt = Clock::now();
		if (!loaded)
		{
			SymCleanup(session);
			fprintf(stderr, "sr-aobscan: %ls: no matching PDB next to the exe\n", exePath);
			return kExitBadInput;
		}

		const Types types(session, kModuleBase);
		std::vector<Answer> answers;
		answers.reserve(keys.size());
		std::map<std::string, std::vector<DWORD>> classes;
		size_t ok = 0;
		for (const Key& key : keys)
		{
			answers.push_back(Resolve(types, key, classes));
			if (strcmp(answers.back().result, "OK") == 0)
				++ok;
		}
		SymCleanup(session);
		const auto doneAt = Clock::now();
		const auto ms = [](Clock::duration d) {
			return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
		};

		if (json)
		{
			printf("{\n  \"summary\": { \"keys\": %zu, \"ok\": %zu, \"failing\": %zu, \"loadMs\": %lld, \"resolveMs\": %lld },\n"
			       "  \"keys\": [\n", keys.size(), ok, keys.size() - ok, ms(loadedAt - start), ms(doneAt - loadedAt));
			for (size_t i = 0; i < keys.size(); ++i)
			{
				const Answer& a = answers[i];
				printf("    { \"key\": \"%s\", \"result\": \"%s\"", JsonEscape(keys[i].text).c_str(), a.result);
				if (strcmp(a.result, "OK") == 0)
				{
					if (!keys[i].member.empty())
						printf(", \"offset\": %llu", static_cast<unsigned long long>(a.offset));
					printf(", \"size\": %llu", static_cast<unsigned long long>(a.size));
					if (!keys[i].member.empty())
						printf(", \"boolMask\": %u", a.boolMask);
				}
				printf(" }%s\n", i + 1 < keys.size() ? "," : "");
			}
			printf("  ]\n}\n");
		}
		else
		{
			printf("%-10s %-64s %8s %8s %4s\n", "RESULT", "KEY", "OFFSET", "SIZE", "MASK");
			for (size_t i = 0; i < keys.size(); ++i)
			{
				const Answer& a = answers[i];
				if (strcmp(a.result, "OK") != 0)
					printf("%-10s %s\n", a.result, keys[i].text.c_str());
				else if (keys[i].member.empty())
					printf("%-10s %-64s %8s %8llX\n", a.result, keys[i].text.c_str(), "-", static_cast<unsigned long long>(a.size));
				else
					printf("%-10s %-64s %8llX %8llX %4X\n", a.result, keys[i].text.c_str(),
						static_cast<unsigned long long>(a.offset), static_cast<unsigned long long>(a.size), a.boolMask);
			}
			printf("%zu keys: %zu OK, %zu failing; PDB load %lld ms, lookups %lld ms\n",
				keys.size(), ok, keys.size() - ok, ms(loadedAt - start), ms(doneAt - loadedAt));
		}
		return ok == keys.size() ? kExitOk : kExitFailed;
	}
}


namespace
{
	std::wstring g_fieldExe;

	// The PDB session --plugin's field requests read, opened on first use.
	struct FieldSession
	{
		HANDLE session = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5355));
		bool   loaded  = false;
		std::unique_ptr<Types> types;
		std::map<std::string, std::vector<DWORD>> classes;
	};

	FieldSession* OpenFieldSession()
	{
		static FieldSession* opened = [] {
			auto* s = new FieldSession;
			s->loaded = !g_fieldExe.empty() && AobScan::LoadMatchedPdb(s->session, g_fieldExe, kModuleBase, 0);
			if (s->loaded)
				s->types = std::make_unique<Types>(s->session, kModuleBase);
			fprintf(stderr, "sr-aobscan: field requests read %s\n",
				s->loaded ? "the exe's PDB" : "nothing: no matching PDB next to the exe");
			return s;
		}();
		return opened;
	}

	VersionMap::LiveField FieldFromPdb(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField live;
		FieldSession* s = OpenFieldSession();
		if (!s->loaded)
		{
			live.error = "no PDB to read the layout from";
			return live;
		}
		// The PDB has no packages: /Script/<Module>.<Name> is looked up by Name.
		std::string name = className;
		if (name.rfind("/Script/", 0) == 0 && name.find('.') != std::string::npos)
			name = name.substr(name.rfind('.') + 1);
		const Key key{ member.empty() ? name : name + "::" + member, name, member };
		const Answer a = Resolve(*s->types, key, s->classes, true);
		if (strcmp(a.result, "OK") != 0)
		{
			live.error = std::string(a.result) + " in the PDB";
			return live;
		}
		live.offset   = static_cast<int32_t>(a.offset);
		live.size     = static_cast<uint32_t>(a.size);
		live.isBool   = a.boolMask != 0;
		live.boolMask = static_cast<uint8_t>(a.boolMask);
		return live;
	}
}

namespace AobScan
{
	void UseLayoutForFields(const wchar_t* exePath)
	{
		g_fieldExe = exePath ? exePath : L"";
		VersionMap::SetLiveFieldSource(FieldFromPdb);
	}
}
