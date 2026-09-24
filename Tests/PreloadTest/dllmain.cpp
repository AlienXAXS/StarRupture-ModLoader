// WIN32_LEAN_AND_MEAN and NOMINMAX come from the project.
#include <Windows.h>

// A preload plugin's DllMain runs during Stage 1, before the game's entry
// point, under the loader lock and with the main thread parked. Keep it empty.
// Anything that allocates, loads another module or waits on something belongs
// in PreloadInit, which the loader calls from an ordinary thread holding no
// lock.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        break;
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
