#pragma once
#include <cstdint>

// Compile-time XOR string obfuscation.
//
// Usage:
//   GetProcAddress(hMod, OBF("PluginInit"));
//
// Each call site gets its own key (derived from __COUNTER__) so identical
// strings at different sites produce different cipher bytes in the binary.
// The decrypted result is a function-scope static — use it immediately.

namespace ObfDetail
{
    template<uint8_t Key, int N>
    struct XorStr
    {
        char buf[N]{};
        constexpr XorStr(const char (&src)[N])
        {
            for (int i = 0; i < N; ++i)
                buf[i] = static_cast<char>(src[i] ^ Key);
        }
        const char* decrypt() const
        {
            static char out[N];
            for (int i = 0; i < N; ++i)
                out[i] = static_cast<char>(buf[i] ^ Key);
            return out;
        }
    };
}

// OBF_IMPL captures the key as a single value so __COUNTER__ is expanded only once.
#define OBF_IMPL(s, key) (ObfDetail::XorStr<static_cast<uint8_t>(key), sizeof(s)>(s).decrypt())
#define OBF(s)           OBF_IMPL(s, 0xA5 ^ (__COUNTER__ & 0xFF))
