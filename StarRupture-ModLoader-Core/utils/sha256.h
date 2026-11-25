#pragma once

// sha256.h -- SHA-256 through BCrypt, as lowercase hex. Header only. Empty on
// any failure.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace Sha256
{
    class Hasher
    {
    public:
        Hasher()
        {
            m_ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
                && BCRYPT_SUCCESS(BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0));
        }
        ~Hasher()
        {
            if (m_hash)
                BCryptDestroyHash(m_hash);
            if (m_algorithm)
                BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }
        Hasher(const Hasher&) = delete;
        Hasher& operator=(const Hasher&) = delete;

        void Add(const void* data, size_t size)
        {
            const UCHAR* bytes = static_cast<const UCHAR*>(data);
            while (m_ok && size > 0)
            {
                const ULONG step = size > 0x40000000 ? 0x40000000 : static_cast<ULONG>(size);
                m_ok = BCRYPT_SUCCESS(BCryptHashData(m_hash, const_cast<PUCHAR>(bytes), step, 0));
                bytes += step;
                size -= step;
            }
        }

        std::string Finish()
        {
            UCHAR digest[32]{};
            if (!m_ok || !BCRYPT_SUCCESS(BCryptFinishHash(m_hash, digest, sizeof(digest), 0)))
                return std::string();
            std::string hex;
            for (const UCHAR byte : digest)
            {
                char pair[3];
                snprintf(pair, sizeof(pair), "%02x", byte);
                hex += pair;
            }
            return hex;
        }

    private:
        BCRYPT_ALG_HANDLE  m_algorithm = nullptr;
        BCRYPT_HASH_HANDLE m_hash      = nullptr;
        bool               m_ok        = false;
    };

    inline std::string OfBytes(const std::string& bytes)
    {
        Hasher hasher;
        hasher.Add(bytes.data(), bytes.size());
        return hasher.Finish();
    }

    // The file is read in chunks.
    inline std::string OfFile(const wchar_t* path)
    {
        constexpr DWORD kChunkBytes = 1 << 20;
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::string();

        Hasher hasher;
        std::vector<UCHAR> chunk(kChunkBytes);
        bool read_ok = true;
        for (;;)
        {
            DWORD read = 0;
            read_ok = ReadFile(file, chunk.data(), kChunkBytes, &read, nullptr) != FALSE;
            if (!read_ok || read == 0)
                break;
            hasher.Add(chunk.data(), read);
        }
        CloseHandle(file);
        return read_ok ? hasher.Finish() : std::string();
    }
}
