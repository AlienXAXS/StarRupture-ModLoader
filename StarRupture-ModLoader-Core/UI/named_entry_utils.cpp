#include "pch.h"
#include "named_entry_utils.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <cstring>
#include <cstdio>

namespace UI::NamedEntryUtils
{
    bool IsValidEntryName(const char* name)
    {
        if (!name || !name[0]) return false;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
        for (const char* p = name; *p; ++p)
            if (strchr("\\/:*?\"<>|", *p)) return false;
        return true;
    }

    static bool NameTaken(const char* candidate, const char* const* existing, int existingCount)
    {
        for (int i = 0; i < existingCount; ++i)
            if (existing[i] && _stricmp(existing[i], candidate) == 0)
                return true;
        return false;
    }

    void SuggestUniqueName(const char* baseName, const char* const* existing,
                            int existingCount, char* out, int outSize)
    {
        if (!out || outSize <= 0) return;
        if (!baseName) baseName = "";

        if (!NameTaken(baseName, existing, existingCount))
        {
            snprintf(out, outSize, "%s", baseName);
            return;
        }

        // " 2", " 3", ... -- 1000 is generous headroom; if every one of
        // those is somehow already taken too, fall back to the base name
        // rather than loop forever (the caller's own save will then just
        // refuse as a duplicate, same as typing it by hand would).
        for (int n = 2; n < 1000; ++n)
        {
            char candidate[128];
            snprintf(candidate, sizeof(candidate), "%s %d", baseName, n);
            if (!NameTaken(candidate, existing, existingCount))
            {
                snprintf(out, outSize, "%s", candidate);
                return;
            }
        }
        snprintf(out, outSize, "%s", baseName);
    }
}

#endif // MODLOADER_CLIENT_BUILD
