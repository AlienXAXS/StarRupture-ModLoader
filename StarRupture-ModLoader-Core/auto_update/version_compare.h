#pragma once

// Version tags as the release workflows write them, compared by number. Used
// by the Core's update checks and by the standalone updater, so the banner,
// the plugin pass and the updater agree on what is newer.
namespace AutoUpdateVersion
{
    struct ParsedVersion
    {
        int  parts[4] = { 0, 0, 0, 0 };
        bool valid = false;
        bool hasSuffix = false; // e.g. "-preview" / "-beta.1"
    };

    // Accepts "v1.15.4", "1.15.2.0", "v1.16.0-beta.1", etc.
    inline ParsedVersion ParseVersion(const char* s)
    {
        ParsedVersion v;
        if (!s || !*s)
            return v;

        const char* p = s;
        if (*p == 'v' || *p == 'V')
            ++p;

        int idx = 0;
        while (idx < 4)
        {
            if (*p < '0' || *p > '9')
                break;
            long val = 0;
            while (*p >= '0' && *p <= '9')
            {
                val = val * 10 + (*p - '0');
                if (val > 1000000) val = 1000000;
                ++p;
            }
            v.parts[idx++] = static_cast<int>(val);
            if (*p == '.')
                ++p;
            else
                break;
        }

        v.valid = (idx >= 2); // require at least major.minor
        v.hasSuffix = (*p == '-' || *p == '+');
        return v;
    }

    // Returns >0 if a is newer than b, 0 if equal, <0 if older (numeric only).
    // A suffix is ignored: 1.23.0-rc1 and 1.23.0 are equal, so a pre-release
    // is never offered its own release and never offered an older one.
    inline int CompareVersions(const ParsedVersion& a, const ParsedVersion& b)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (a.parts[i] != b.parts[i])
                return a.parts[i] < b.parts[i] ? -1 : 1;
        }
        return 0;
    }
}
