#include "dwmapi_proxy.h"
#include "logging/log.h"

// ---------------------------------------------------------------------------
// Real dwmapi.dll handle + ordinal-indexed function table
// Index == ordinal (ordinals start at Base, typically 1 on dwmapi.dll)
// Size 251 covers the full ordinal range seen on any Windows version (1-198+)
// ---------------------------------------------------------------------------

static HMODULE g_realDwmapiDll = nullptr;
static FARPROC g_ordinalFuncs[251] = {};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool DwmapiProxy::Initialize()
{
	LogToFile::Info("DwmapiProxy::Initialize() starting");

	wchar_t systemDir[MAX_PATH]{};
	GetSystemDirectoryW(systemDir, MAX_PATH);
	LogToFile::Debug("System directory: %ls", systemDir);

	wchar_t realPath[MAX_PATH]{};
	swprintf_s(realPath, L"%s\\dwmapi.dll", systemDir);
	LogToFile::Info("Loading real dwmapi.dll from: %ls", realPath);

	g_realDwmapiDll = LoadLibraryW(realPath);
	if (!g_realDwmapiDll)
	{
		LogToFile::Error("LoadLibraryW failed for real dwmapi.dll (error %lu)", GetLastError());
		return false;
	}

	LogToFile::Info("Real dwmapi.dll loaded at 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realDwmapiDll)));

	// Walk the PE export directory — works for any Windows version automatically
	auto* base      = reinterpret_cast<BYTE*>(g_realDwmapiDll);
	auto* dosHdr    = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	auto* ntHdrs    = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHdr->e_lfanew);
	auto& expDirDE  = ntHdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

	if (expDirDE.VirtualAddress == 0)
	{
		LogToFile::Error("Real dwmapi.dll has no export directory — this is unexpected");
		return false;
	}

	auto* expDir   = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + expDirDE.VirtualAddress);
	auto* funcRVAs = reinterpret_cast<DWORD*>(base + expDir->AddressOfFunctions);
	DWORD ordBase  = expDir->Base;          // first valid ordinal (usually 1)
	DWORD numFuncs = expDir->NumberOfFunctions;

	LogToFile::Info("PE export table: base ordinal %lu, %lu function slots", ordBase, numFuncs);

	int resolved = 0, skipped = 0;
	for (DWORD i = 0; i < numFuncs; ++i)
	{
		if (funcRVAs[i] == 0) { ++skipped; continue; }

		DWORD ordinal = ordBase + i;
		if (ordinal >= 251)
		{
			LogToFile::Warn("  Ordinal %lu is out of range (>= 251) — skipped", ordinal);
			continue;
		}

		g_ordinalFuncs[ordinal] = reinterpret_cast<FARPROC>(base + funcRVAs[i]);
		LogToFile::Debug("  [ord %3lu] -> 0x%llX", ordinal,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_ordinalFuncs[ordinal])));
		++resolved;
	}

	LogToFile::Info("Export resolution complete: %d resolved, %d null slots skipped", resolved, skipped);
	return true;
}

void DwmapiProxy::Shutdown()
{
	LogToFile::Info("DwmapiProxy::Shutdown() starting");

	if (g_realDwmapiDll)
	{
		LogToFile::Debug("Freeing real dwmapi.dll (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realDwmapiDll)));
		FreeLibrary(g_realDwmapiDll);
		g_realDwmapiDll = nullptr;
		LogToFile::Info("Real dwmapi.dll unloaded");
	}
	else
	{
		LogToFile::Debug("Real dwmapi.dll was already null — nothing to free");
	}

	LogToFile::Info("DwmapiProxy::Shutdown() complete");
}

// ---------------------------------------------------------------------------
// Forwarding stubs — one per ordinal 1-198
// The .def file maps named exports and NONAME ordinal-only exports to these.
// ---------------------------------------------------------------------------

extern "C"
{

typedef DWORD_PTR(WINAPI* GenericFunc)(
	DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR,
	DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR);

#define FORWARD_ORD(N)                                                              \
	__declspec(dllexport) DWORD_PTR WINAPI vp_ord##N(                              \
		DWORD_PTR a1, DWORD_PTR a2, DWORD_PTR a3, DWORD_PTR a4,                    \
		DWORD_PTR a5, DWORD_PTR a6, DWORD_PTR a7, DWORD_PTR a8)                    \
	{                                                                               \
		if (!g_ordinalFuncs[N]) return 0;                                           \
		return reinterpret_cast<GenericFunc>(g_ordinalFuncs[N])(                    \
			a1, a2, a3, a4, a5, a6, a7, a8);                                       \
	}

// Ordinals 1-99 (named exports on Windows 11; gaps return 0 safely)
FORWARD_ORD(1)   FORWARD_ORD(2)   FORWARD_ORD(3)   FORWARD_ORD(4)
FORWARD_ORD(5)   FORWARD_ORD(6)   FORWARD_ORD(7)   FORWARD_ORD(8)
FORWARD_ORD(9)   FORWARD_ORD(10)  FORWARD_ORD(11)  FORWARD_ORD(12)
FORWARD_ORD(13)  FORWARD_ORD(14)  FORWARD_ORD(15)  FORWARD_ORD(16)
FORWARD_ORD(17)  FORWARD_ORD(18)  FORWARD_ORD(19)  FORWARD_ORD(20)
FORWARD_ORD(21)  FORWARD_ORD(22)  FORWARD_ORD(23)  FORWARD_ORD(24)
FORWARD_ORD(25)  FORWARD_ORD(26)  FORWARD_ORD(27)  FORWARD_ORD(28)
FORWARD_ORD(29)  FORWARD_ORD(30)  FORWARD_ORD(31)  FORWARD_ORD(32)
FORWARD_ORD(33)  FORWARD_ORD(34)  FORWARD_ORD(35)  FORWARD_ORD(36)
FORWARD_ORD(37)  FORWARD_ORD(38)  FORWARD_ORD(39)  FORWARD_ORD(40)
FORWARD_ORD(41)  FORWARD_ORD(42)  FORWARD_ORD(43)  FORWARD_ORD(44)
FORWARD_ORD(45)  FORWARD_ORD(46)  FORWARD_ORD(47)  FORWARD_ORD(48)
FORWARD_ORD(49)  FORWARD_ORD(50)  FORWARD_ORD(51)  FORWARD_ORD(52)
FORWARD_ORD(53)  FORWARD_ORD(54)  FORWARD_ORD(55)  FORWARD_ORD(56)
FORWARD_ORD(57)  FORWARD_ORD(58)  FORWARD_ORD(59)  FORWARD_ORD(60)
FORWARD_ORD(61)  FORWARD_ORD(62)  FORWARD_ORD(63)  FORWARD_ORD(64)
FORWARD_ORD(65)  FORWARD_ORD(66)  FORWARD_ORD(67)  FORWARD_ORD(68)
FORWARD_ORD(69)  FORWARD_ORD(70)  FORWARD_ORD(71)  FORWARD_ORD(72)
FORWARD_ORD(73)  FORWARD_ORD(74)  FORWARD_ORD(75)  FORWARD_ORD(76)
FORWARD_ORD(77)  FORWARD_ORD(78)  FORWARD_ORD(79)  FORWARD_ORD(80)
FORWARD_ORD(81)  FORWARD_ORD(82)  FORWARD_ORD(83)  FORWARD_ORD(84)
FORWARD_ORD(85)  FORWARD_ORD(86)  FORWARD_ORD(87)  FORWARD_ORD(88)
FORWARD_ORD(89)  FORWARD_ORD(90)  FORWARD_ORD(91)  FORWARD_ORD(92)
FORWARD_ORD(93)  FORWARD_ORD(94)  FORWARD_ORD(95)  FORWARD_ORD(96)
FORWARD_ORD(97)  FORWARD_ORD(98)  FORWARD_ORD(99)

// Ordinals 100-198 (unnamed / ordinal-only on Windows 10)
FORWARD_ORD(100) FORWARD_ORD(101) FORWARD_ORD(102) FORWARD_ORD(103)
FORWARD_ORD(104) FORWARD_ORD(105) FORWARD_ORD(106) FORWARD_ORD(107)
FORWARD_ORD(108) FORWARD_ORD(109) FORWARD_ORD(110) FORWARD_ORD(111)
FORWARD_ORD(112) FORWARD_ORD(113) FORWARD_ORD(114) FORWARD_ORD(115)
FORWARD_ORD(116) FORWARD_ORD(117) FORWARD_ORD(118) FORWARD_ORD(119)
FORWARD_ORD(120) FORWARD_ORD(121) FORWARD_ORD(122) FORWARD_ORD(123)
FORWARD_ORD(124) FORWARD_ORD(125) FORWARD_ORD(126) FORWARD_ORD(127)
FORWARD_ORD(128) FORWARD_ORD(129) FORWARD_ORD(130) FORWARD_ORD(131)
FORWARD_ORD(132) FORWARD_ORD(133) FORWARD_ORD(134) FORWARD_ORD(135)
FORWARD_ORD(136) FORWARD_ORD(137) FORWARD_ORD(138) FORWARD_ORD(139)
FORWARD_ORD(140) FORWARD_ORD(141) FORWARD_ORD(142) FORWARD_ORD(143)
FORWARD_ORD(144) FORWARD_ORD(145) FORWARD_ORD(146) FORWARD_ORD(147)
FORWARD_ORD(148) FORWARD_ORD(149) FORWARD_ORD(150) FORWARD_ORD(151)
FORWARD_ORD(152) FORWARD_ORD(153) FORWARD_ORD(154) FORWARD_ORD(155)
FORWARD_ORD(156) FORWARD_ORD(157) FORWARD_ORD(158) FORWARD_ORD(159)
FORWARD_ORD(160) FORWARD_ORD(161) FORWARD_ORD(162) FORWARD_ORD(163)
FORWARD_ORD(164) FORWARD_ORD(165) FORWARD_ORD(166) FORWARD_ORD(167)
FORWARD_ORD(168) FORWARD_ORD(169) FORWARD_ORD(170) FORWARD_ORD(171)
FORWARD_ORD(172) FORWARD_ORD(173) FORWARD_ORD(174) FORWARD_ORD(175)
FORWARD_ORD(176) FORWARD_ORD(177) FORWARD_ORD(178) FORWARD_ORD(179)
FORWARD_ORD(180) FORWARD_ORD(181) FORWARD_ORD(182) FORWARD_ORD(183)
FORWARD_ORD(184) FORWARD_ORD(185) FORWARD_ORD(186) FORWARD_ORD(187)
FORWARD_ORD(188) FORWARD_ORD(189) FORWARD_ORD(190) FORWARD_ORD(191)
FORWARD_ORD(192) FORWARD_ORD(193) FORWARD_ORD(194) FORWARD_ORD(195)
FORWARD_ORD(196) FORWARD_ORD(197) FORWARD_ORD(198)

} // extern "C"
