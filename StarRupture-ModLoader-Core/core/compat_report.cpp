#include "compat_report.h"

#include <cctype>
#include <cstdio>

namespace CompatReport
{
    namespace
    {
        // Bumped on any change to the report's fields.
        constexpr int kReportFormat = 2;

        void AppendString(std::string& out, const std::string& text)
        {
            out += '"';
            for (const unsigned char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += static_cast<char>(c);
                }
                else if (c < 0x20)
                {
                    char escaped[8];
                    snprintf(escaped, sizeof(escaped), "\\u%04X", c);
                    out += escaped;
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
            out += '"';
        }

        const char* Bool(bool value)
        {
            return value ? "true" : "false";
        }

        // "key": [ one item per line ],
        template <class T, class AppendItem>
        void AppendArray(std::string& out, const char* key, const std::vector<T>& items, AppendItem appendItem, bool last)
        {
            out += "  \"";
            out += key;
            out += "\": [";
            for (size_t i = 0; i < items.size(); ++i)
            {
                out += i ? ",\n    { " : "\n    { ";
                appendItem(out, items[i]);
                out += " }";
            }
            out += items.empty() ? "]" : "\n  ]";
            out += last ? "\n" : ",\n";
        }
    }

    std::string ClOf(const std::string& productVersion)
    {
        constexpr char kClTag[] = "CL-";
        const size_t at = productVersion.rfind(kClTag);
        if (at == std::string::npos)
            return "unknown";

        std::string cl;
        for (size_t i = at + sizeof(kClTag) - 1; i < productVersion.size() && std::isdigit(static_cast<unsigned char>(productVersion[i])); ++i)
            cl += productVersion[i];
        return cl.empty() ? "unknown" : cl;
    }

    std::string Build(const Inputs& inputs)
    {
        const LayoutCheck::Result& layout = *inputs.layout;

        std::string out = "{\n";
        out += "  \"format\": " + std::to_string(kReportFormat) + ",\n";
        out += "  \"productVersion\": ";
        AppendString(out, inputs.productVersion);
        out += ",\n  \"exe\": { \"name\": ";
        AppendString(out, inputs.exeName);
        out += ", \"sha256\": ";
        AppendString(out, inputs.exeSha256);
        out += " },\n";
        out += "  \"sdkCL\": " + std::to_string(inputs.sdkCL) + ",\n";
        out += "  \"layout\": { \"classes\": " + std::to_string(layout.classes)
            + ", \"checked\": " + std::to_string(layout.checked)
            + ", \"mismatched\": " + std::to_string(layout.mismatched)
            + ", \"missing\": " + std::to_string(layout.missing)
            + ", \"deferred\": " + std::to_string(layout.deferred) + " },\n";

        AppendArray(out, "patternMisses", inputs.patternMisses, [&inputs](std::string& o, const PatternMiss& miss)
        {
            o += "\"name\": ";
            AppendString(o, miss.name);
            o += ", \"module\": ";
            AppendString(o, inputs.exeName);
            o += ", \"pattern\": ";
            AppendString(o, miss.pattern);
            o += std::string(", \"required\": ") + Bool(miss.required) + ", \"found\": " + Bool(miss.found);
        }, false);

        AppendArray(out, "globals", inputs.globals, [](std::string& o, const GlobalRva& global)
        {
            o += "\"name\": ";
            AppendString(o, global.name);
            o += std::string(", \"resolved\": ") + Bool(global.resolved)
                + ", \"rva\": " + std::to_string(global.rva)
                + ", \"sdkOffset\": " + std::to_string(global.sdkOffset)
                + ", \"rejected\": " + Bool(global.rejected);
        }, false);

        AppendArray(out, "changes", layout.changes, [](std::string& o, const LayoutCheck::Change& change)
        {
            o += "\"class\": ";
            AppendString(o, change.className);
            o += ", \"member\": ";
            AppendString(o, change.member);
            o += ", \"field\": ";
            AppendString(o, change.field);
            if (change.field != LayoutCheck::kChangeMissing)
                o += ", \"sdk\": " + std::to_string(change.sdk) + ", \"game\": " + std::to_string(change.game);
        }, true);

        out += "}\n";
        return out;
    }
}

#if defined(MODLOADER_CLIENT_BUILD)

#include "layout_table.generated.h"
#include "sdk_globals.h"
#include "startup_utils.h"
#include "version_check.h"
#include "../logging/logger.h"
#include "../memory_scanner/pattern_preflight.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace CompatReport
{
    namespace
    {
        constexpr DWORD  kHashChunkBytes = 1 << 20;
        constexpr size_t kSha256Bytes    = 32;

        std::string WideToUtf8(const std::wstring& text)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return std::string();
            std::string out(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        // Lowercase hex SHA-256 of a file, read in chunks; empty on any failure.
        std::string Sha256OfFile(const wchar_t* path)
        {
            const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return std::string();

            BCRYPT_ALG_HANDLE  algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash      = nullptr;
            UCHAR digest[kSha256Bytes]{};
            bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
                && BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));

            std::vector<UCHAR> chunk(kHashChunkBytes);
            while (ok)
            {
                DWORD read = 0;
                ok = ReadFile(file, chunk.data(), kHashChunkBytes, &read, nullptr) != FALSE;
                if (!ok || read == 0)
                    break;
                ok = BCRYPT_SUCCESS(BCryptHashData(hash, chunk.data(), read, 0));
            }
            ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));

            if (hash)
                BCryptDestroyHash(hash);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
            CloseHandle(file);

            if (!ok)
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
    }

    std::wstring Write(const LayoutCheck::Result& layout)
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const wchar_t* lastSlash = wcsrchr(exePath, L'\\');

        Inputs inputs{ WideToUtf8(GetGameVersionString()), WideToUtf8(lastSlash ? lastSlash + 1 : exePath),
            Sha256OfFile(exePath), SrLayoutTable::kBuiltCL, {}, {}, &layout };
        for (const PatternPreflight::Miss& miss : PatternPreflight::Misses())
            inputs.patternMisses.push_back(PatternMiss{ miss.name, miss.pattern, miss.required, miss.structural });
        for (const SdkGlobals::Resolution& global : SdkGlobals::Resolutions())
            inputs.globals.push_back(GlobalRva{ global.name, global.resolved, global.rva, global.sdkOffset, global.rejected });

        const std::string cl = ClOf(inputs.productVersion);
        const std::wstring fileName = L"compat-report-" + std::wstring(cl.begin(), cl.end()) + L".json";

        std::ofstream file(GetModLoaderDirPath(fileName.c_str()), std::ios::binary | std::ios::trunc);
        file << Build(inputs);
        file.close();
        if (!file)
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Could not write the compatibility report ModLoader\\%s", fileName.c_str());
            return std::wstring();
        }

        ModLoaderLogger::LogInfo(L"[LayoutCheck] Compatibility report written to ModLoader\\%s", fileName.c_str());
        return fileName;
    }
}

#endif
