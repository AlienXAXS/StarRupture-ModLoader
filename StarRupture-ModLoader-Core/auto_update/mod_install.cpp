#include "mod_install.h"
#include "auto_update/vortex_notice.h"
#include "auto_update/http_get.h"
#include "logging/logger.h"
#include "utils/sha256.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ModInstall
{
	namespace
	{
		constexpr const char kRefusedAddress[] = "The download was sent to an address that is not allowed, so nothing was installed.";
		constexpr const char kFailed[]         = "The download failed. Check your connection and try again.";
		constexpr const char kWrongFile[]      = "The downloaded file does not match the mod list, so nothing was installed.";
		constexpr const char kWriteFailed[]    = "The mod's files could not be written. Nothing was installed.";
		constexpr const char kCancelled[]      = "The install was stopped.";
		constexpr const char kTempSuffix[]     = ".install.tmp";
		constexpr int        kMoveRetries      = 3;
		constexpr DWORD      kMoveRetryMs      = 200;

		// The two hosts GitHub serves release assets from.
		const char* const kDownloadHosts[] = { "release-assets.githubusercontent.com", "objects.githubusercontent.com" };

		// The host of an https URL that has no user info or port, else empty.
		std::string HttpsHost(const std::string& url)
		{
			if (url.compare(0, 8, "https://") != 0)
				return {};
			for (const char c : url)
				if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F || c == '\\')
					return {};
			const size_t end = url.find_first_of("/?#", 8);
			const std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
			return host.find_first_of("@:%") == std::string::npos ? host : std::string();
		}

		bool IsDownloadHost(const std::string& host)
		{
			for (const char* allowed : kDownloadHosts)
				if (ModIndex::detail::EqualsNoCase(host, allowed))
					return true;
			return false;
		}

		std::wstring Wide(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

		bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

		std::string SidecarName(const std::string& dllFile) { return dllFile.substr(0, dllFile.size() - 4) + ".json"; }

		// The sidecar's whole text. The URL passed IsPluginDllName's rules and holds nothing to escape.
		std::string SidecarBody(const std::string& manifestUrl)
		{
			return "{\n  \"manifest_url\": \"" + manifestUrl + "\"\n}\n";
		}

		// Whether the file at `path` holds exactly `text`.
		bool HoldsText(const std::wstring& path, const std::string& text)
		{
			const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			std::string read(text.size() + 1, '\0');
			DWORD got = 0;
			const bool ok = ReadFile(file, &read[0], static_cast<DWORD>(read.size()), &got, nullptr) != FALSE;
			CloseHandle(file);
			read.resize(ok ? got : 0);
			return ok && read == text;
		}

		// A rename that is tried again a few times when a virus scanner or indexer
		// still holds the new file. Any other error, such as a name that is already
		// taken, is final.
		DWORD MoveWithRetry(const PluginManage::MoveFn& move, const std::wstring& from, const std::wstring& to)
		{
			DWORD error = move(from, to);
			for (int retry = 0; retry < kMoveRetries
				&& (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED); ++retry)
			{
				Sleep(kMoveRetryMs);
				error = move(from, to);
			}
			return error;
		}

		// A temp file of our own. One left by an earlier attempt is deleted, and
		// CREATE_NEW never writes through whatever else is at that name.
		bool WriteTemp(const std::wstring& path, const std::string& bytes)
		{
			DeleteFileW(path.c_str());
			const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD written = 0;
			const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
				&& written == bytes.size() && FlushFileBuffers(file);
			CloseHandle(file);
			if (!ok)
				DeleteFileW(path.c_str());
			return ok;
		}
	}

	HopFn HttpHop()
	{
		return [](const std::string& url, size_t maxBytes, unsigned deadlineMs, const std::atomic<bool>& cancel)
		{
			Hop hop;
			HttpGetOptions options;
			options.maxBytes = maxBytes;
			options.deadlineMs = deadlineMs;
			options.noRedirect = true;
			options.location = &hop.location;
			options.status = &hop.status;
			options.cancel = &cancel;
			hop.body = HttpGet(url.c_str(), "mod install", options);
			return hop;
		};
	}

	bool IsAllowedRedirect(const std::string& from, const std::string& to)
	{
		const std::string fromHost = HttpsHost(from);
		return (ModIndex::detail::EqualsNoCase(fromHost, "github.com") || IsDownloadHost(fromHost))
			&& IsDownloadHost(HttpsHost(to));
	}

	bool Download(const ModIndex::InstallInfo& info, const std::string& label, const HopFn& hop,
		const std::atomic<bool>& cancel, std::string& bytes, std::string& why)
	{
		bytes.clear();
		if (!ModIndex::IsReleaseAssetUrl(info.url) || info.size == 0
			|| info.size > static_cast<size_t>(ModIndex::kMaxDownloadBytes))
		{
			why = kRefusedAddress;
			return false;
		}

		const ULONGLONG started = GetTickCount64();
		std::string url = info.url;
		for (int redirects = 0;; ++redirects)
		{
			const ULONGLONG spent = GetTickCount64() - started;
			if (cancel.load() || spent >= kDeadlineMs)
			{
				why = cancel.load() ? kCancelled : kFailed;
				return false;
			}
			Hop answer = hop(url, info.size, kDeadlineMs - static_cast<unsigned>(spent), cancel);
			if (cancel.load())
			{
				why = kCancelled;
				return false;
			}
			if (answer.status == 200)
			{
				bytes = std::move(answer.body);
				break;
			}
			const unsigned s = answer.status;
			if (s != 301 && s != 302 && s != 303 && s != 307 && s != 308)
			{
				why = kFailed;
				return false;
			}
			if (redirects >= kMaxRedirects || !IsAllowedRedirect(url, answer.location))
			{
				// The host only: the rest of a download address can carry a key.
				const std::string host = HttpsHost(answer.location);
				ModLoaderLogger::LogWarn(L"[Install] %hs: refused redirect to %hs", label.c_str(),
					host.empty() ? "an address that is not a plain https one" : host.c_str());
				why = kRefusedAddress;
				return false;
			}
			url = std::move(answer.location);
		}

		if (bytes.size() != info.size)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: got %zu bytes, expected %zu; nothing written",
				label.c_str(), bytes.size(), info.size);
			// No body: the request itself failed.
			why = bytes.empty() ? kFailed : kWrongFile;
			bytes.clear();
			return false;
		}
		const std::string digest = Sha256::OfBytes(bytes);
		if (digest.empty() || digest != info.sha256)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: SHA-256 mismatch: expected %hs, got %hs; nothing written",
				label.c_str(), info.sha256.c_str(), digest.c_str());
			bytes.clear();
			why = kWrongFile;
			return false;
		}
		ModLoaderLogger::LogInfo(L"[Install] %hs: SHA-256 matches %hs", label.c_str(), digest.c_str());
		return true;
	}

	std::string CheckTarget(const Request& request)
	{
		const std::string& dll = request.info.dllFile;
		if (!ModIndex::detail::IsInstallDllName(dll))
			return kWriteFailed;
		// Same rule as uninstall and restore: a linked Plugins folder is never written to.
		if (PluginManage::IsReparsePoint(request.pluginsDir))
			return "ModLoader\\Plugins is a link, so nothing was installed.";

		const std::string stem = "ModLoader\\Plugins\\" + dll.substr(0, dll.size() - 4);
		if (VortexNotice::ReadDeployment(request.gameDir).Lists({ stem + ".dll", stem + ".pdb", stem + ".json" })
			== VortexNotice::Listing::Listed)
			return kVortexManaged;

		if (Exists(request.pluginsDir + L"\\" + Wide(dll)))
			return dll + " is already in ModLoader\\Plugins, so nothing was installed.";
		// A sidecar left by an install that was stopped between its two renames has
		// no DLL beside it and holds exactly what this install would write: it is
		// ours, and Place replaces it. Any other X.json is the player's.
		const std::string json = SidecarName(dll);
		const std::wstring jsonPath = request.pluginsDir + L"\\" + Wide(json);
		if (!request.info.manifestUrl.empty() && Exists(jsonPath)
			&& !HoldsText(jsonPath, SidecarBody(request.info.manifestUrl)))
			return json + " is already in ModLoader\\Plugins, so nothing was installed.";
		return {};
	}

	void ClearStranded(const Request& request)
	{
		const std::wstring json = request.pluginsDir + L"\\" + Wide(SidecarName(request.info.dllFile));
		if (!request.info.manifestUrl.empty() && HoldsText(json, SidecarBody(request.info.manifestUrl)))
			DeleteFileW(json.c_str());
	}

	bool Place(const Request& request, const std::string& bytes, std::string& why, const PluginManage::MoveFn& move)
	{
		why = CheckTarget(request);
		if (!why.empty())
			return false;

		const bool sidecar = !request.info.manifestUrl.empty();
		const std::wstring dll = request.pluginsDir + L"\\" + Wide(request.info.dllFile);
		const std::wstring json = request.pluginsDir + L"\\" + Wide(SidecarName(request.info.dllFile));
		const std::wstring dllTemp = dll + Wide(kTempSuffix);
		const std::wstring jsonTemp = json + Wide(kTempSuffix);

		CreateDirectoryW(request.pluginsDir.c_str(), nullptr);

		bool ok = WriteTemp(dllTemp, bytes)
			&& (!sidecar || WriteTemp(jsonTemp, SidecarBody(request.info.manifestUrl)));
		// The sidecar goes first, so the DLL is never there without it.
		bool jsonPlaced = false;
		DWORD error = ok ? ERROR_SUCCESS : GetLastError();
		if (ok && sidecar)
		{
			ClearStranded(request);
			error = MoveWithRetry(move, jsonTemp, json);
			ok = jsonPlaced = error == ERROR_SUCCESS;
		}
		if (ok)
		{
			error = MoveWithRetry(move, dllTemp, dll);
			ok = error == ERROR_SUCCESS;
		}
		if (ok)
			return true;

		if (jsonPlaced)
			DeleteFileW(json.c_str());
		DeleteFileW(dllTemp.c_str());
		DeleteFileW(jsonTemp.c_str());
		ModLoaderLogger::LogWarn(L"[Install] %hs: could not write the files (error %lu)", request.name.c_str(), error);
		why = kWriteFailed;
		return false;
	}

	std::string RequirementMessage(const std::string& modName, const std::vector<Requirement>& needs)
	{
		for (const Requirement& need : needs)
		{
			if (need.state == Need::Missing)
				return modName + " needs " + need.name + ". Install " + need.name + " first.";
			if (need.state == Need::Disabled)
				return modName + " needs " + need.name + ". Enable " + need.name + " first.";
		}
		return {};
	}

	struct Installer::Shared
	{
		std::mutex mutex;
		std::condition_variable idle;
		std::atomic<bool> cancel{ false };
		bool running = false;
		std::atomic<bool> stopped{ false };   // set under the mutex; read without it
		HopFn hop;
	};

	namespace
	{
		void Work(std::shared_ptr<Installer::Shared> shared, Request request, DoneFn done)
		{
			Outcome outcome;
			outcome.dllFile = request.info.dllFile;
			outcome.version = request.version;
			const char* const trust = request.listSigned ? "signed" : "not signed (ModIndexUrl)";
			std::string bytes;
			bool downloaded = false;
			try
			{
				ModLoaderLogger::LogInfo(L"[Install] %hs: downloading %hs (%zu bytes), mod list %hs",
					request.name.c_str(), request.info.url.c_str(), request.info.size, trust);
				outcome.message = CheckTarget(request);
				downloaded = outcome.message.empty()
					&& Download(request.info, request.name, shared->hop, shared->cancel, bytes, outcome.message);
			}
			catch (...)
			{
				outcome.message = kFailed;
			}

			// Writing and reporting happen under the lock Shutdown takes, so
			// neither happens after it has returned.
			std::lock_guard<std::mutex> lock(shared->mutex);
			if (!shared->stopped)
			{
				try
				{
					outcome.placed = downloaded && Place(request, bytes, outcome.message);
				}
				catch (...)
				{
					outcome.message = kWriteFailed;
				}
				if (outcome.placed)
					ModLoaderLogger::LogInfo(L"[Install] %hs: %hs written, mod list %hs",
						request.name.c_str(), request.info.dllFile.c_str(), trust);
				else
					ModLoaderLogger::LogWarn(L"[Install] %hs: failed: %hs", request.name.c_str(), outcome.message.c_str());
				shared->running = false;
				try { done(outcome); }
				catch (...) {}
			}
			shared->running = false;
			shared->idle.notify_all();
		}
	}

	Installer::Installer(HopFn hop)
		: m_shared(std::make_shared<Shared>())
	{
		m_shared->hop = std::move(hop);
	}

	Installer::~Installer()
	{
		Shutdown(0);
	}

	bool Installer::Start(Request request, DoneFn done)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		if (m_shared->stopped || m_shared->running)
			return false;
		m_shared->running = true;
		try
		{
			std::thread(&Work, m_shared, std::move(request), std::move(done)).detach();
		}
		catch (const std::exception&)
		{
			m_shared->running = false;
			return false;
		}
		return true;
	}

	bool Installer::Halted() const
	{
		return m_shared->stopped;
	}

	bool Installer::Busy() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		return m_shared->running;
	}

	bool Installer::Shutdown(unsigned waitMs)
	{
		m_shared->cancel = true;
		std::unique_lock<std::mutex> lock(m_shared->mutex);
		m_shared->stopped = true;
		if (waitMs)
			m_shared->idle.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !m_shared->running; });
		return !m_shared->running;
	}

	Installer& Instance()
	{
		static Installer installer(HttpHop());
		return installer;
	}
}
