#include "utils/open_page.h"

#include "auto_update/mod_index.h"
#include "logging/logger.h"

#include <windows.h>
#include <shellapi.h>

namespace OpenPage
{
	bool Open(const std::string& url, const std::function<void(const std::wstring&)>& opener)
	{
		if (!ModIndex::IsAllowedLinkUrl(url))
			return false;
		const std::wstring wide(url.begin(), url.end()); // ASCII only, by the same check
		opener(wide);
		return true;
	}

	bool Open(const std::string& url)
	{
		return Open(url, [](const std::wstring& wide)
		{
			// ShellExecuteW reports a failure as a value of 32 or less.
			const INT_PTR result = reinterpret_cast<INT_PTR>(
				ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
			if (result <= 32)
				ModLoaderLogger::LogWarn(L"[OpenPage] Opening %ls failed (code %d)", wide.c_str(), static_cast<int>(result));
		});
	}
}
