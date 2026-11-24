#include "utils/open_page.h"

#include "auto_update/mod_index.h"

#include <windows.h>
#include <shellapi.h>

namespace OpenPage
{
	bool Open(const std::string& url, const std::function<void(const std::wstring&)>& opener)
	{
		if (!ModIndex::IsAllowedPageUrl(url))
			return false;
		const std::wstring wide(url.begin(), url.end()); // ASCII only, by the same check
		opener(wide);
		return true;
	}

	bool Open(const std::string& url)
	{
		return Open(url, [](const std::wstring& wide)
		{
			ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		});
	}
}
