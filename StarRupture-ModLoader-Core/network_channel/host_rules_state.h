#pragma once
#include <atomic>

// What IPluginNetworkChannel::GetHostRulesState answers, kept apart from the
// network code so its ordering can be tested. InEffect is called only after the
// plugins' flags were written from the joined server's rules, and Cleared before
// they are handed back, so on the game thread a 1 never goes with a stale flag.
namespace HostRulesWire
{
    class RulesStateCell
    {
    public:
        void Greeted(bool on) { m_greeted.store(on, std::memory_order_release); }
        void InEffect()       { m_inEffect.store(true, std::memory_order_release); }
        void Cleared()        { m_inEffect.store(false, std::memory_order_release); }

        int Value() const
        {
            if (m_inEffect.load(std::memory_order_acquire)) return 1;
            return m_greeted.load(std::memory_order_acquire) ? 0 : -1;
        }

    private:
        std::atomic<bool> m_greeted{ false };
        std::atomic<bool> m_inEffect{ false };
    };
}
