#pragma once

#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// host_rules.h -- the server's "turn this feature off" list, as plain data.
//
// A server (or its operator) can name a feature of a plugin, by the plugin's
// name and the feature's id, that the players on it must not use. The loader
// keeps the list as a deny set and works out, whenever it changes, which
// features to switch off and which to hand back. This header holds only that
// arithmetic: it knows nothing about the loader, the game or Windows, so it can
// be tested on its own.
//
// What a deny set does NOT do: it never turns a feature on. A feature that is
// off for the running game build (a key did not resolve, the class is not
// loaded yet, the build does not have it) stays off whatever the set says.
// EffectiveOn is that rule.
// ---------------------------------------------------------------------------
namespace HostRules
{
    // One denied feature. The plugin name compares without regard to case, as
    // the plugin list and the ini do; the feature id compares exactly, as the
    // plugin wrote it.
    struct RuleKey
    {
        std::string plugin;
        std::string id;
    };

    using DenySet = std::vector<RuleKey>;

    struct DenyDiff
    {
        DenySet added;    // in `wanted`, not in `old`
        DenySet removed;  // in `old`, not in `wanted`
    };

    inline char LowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline bool SamePlugin(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (LowerAscii(a[i]) != LowerAscii(b[i]))
                return false;
        return true;
    }

    inline bool SameRule(const RuleKey& a, const RuleKey& b)
    {
        return a.id == b.id && SamePlugin(a.plugin, b.plugin);
    }

    inline bool Contains(const DenySet& set, const std::string& plugin, const std::string& id)
    {
        for (const RuleKey& r : set)
            if (r.id == id && SamePlugin(r.plugin, plugin))
                return true;
        return false;
    }

    // The set without empty entries (no plugin or no id) and without repeats,
    // in first-seen order.
    inline DenySet Normalize(const DenySet& in)
    {
        DenySet out;
        for (const RuleKey& r : in)
        {
            if (r.plugin.empty() || r.id.empty())
                continue;
            if (Contains(out, r.plugin, r.id))
                continue;
            out.push_back(r);
        }
        return out;
    }

    inline DenyDiff DiffDeny(const DenySet& old, const DenySet& wanted)
    {
        DenyDiff d;
        for (const RuleKey& r : wanted)
            if (!Contains(old, r.plugin, r.id))
                d.added.push_back(r);
        for (const RuleKey& r : old)
            if (!Contains(wanted, r.plugin, r.id))
                d.removed.push_back(r);
        return d;
    }

    // Where a deny list comes from. `local` is what the console on this machine
    // set (features hostoff); `remote` is what the server this client is joined
    // to sent. They are kept apart so a received set replaces only the remote
    // part and leaving the server drops only that: a local entry survives both.
    // What is switched off is the two together.
    struct DenySources
    {
        DenySet local;
        DenySet remote;

        DenySet Effective() const
        {
            DenySet all = local;
            all.insert(all.end(), remote.begin(), remote.end());
            return Normalize(all);
        }
    };

    // Whether a feature runs: it is on for the build AND the server does not
    // deny it. The server can only take a feature away, never give one back.
    inline bool EffectiveOn(bool buildOn, bool hostOff)
    {
        return buildOn && !hostOff;
    }
}
