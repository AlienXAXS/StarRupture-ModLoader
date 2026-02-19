// The following ifdef block is the standard way of creating macros which make exporting
// from a DLL simpler. All files within this DLL are compiled with the RAILJUNCTIONFIXER_EXPORTS
// symbol defined on the command line. This symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see
// RAILJUNCTIONFIXER_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.
#ifdef RAILJUNCTIONFIXER_EXPORTS
#define RAILJUNCTIONFIXER_API __declspec(dllexport)
#else
#define RAILJUNCTIONFIXER_API __declspec(dllimport)
#endif

// This class is exported from the dll
class RAILJUNCTIONFIXER_API CRailJunctionFixer {
public:
	CRailJunctionFixer(void);
	// TODO: add your methods here.
};

extern RAILJUNCTIONFIXER_API int nRailJunctionFixer;

RAILJUNCTIONFIXER_API int fnRailJunctionFixer(void);
