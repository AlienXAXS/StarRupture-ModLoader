#include "mod_install.h"
#include "auto_update/vortex_notice.h"
#include "auto_update/http_get.h"
#include "auto_update/version_compare.h"
#include "logging/logger.h"
#include "plugins/plugin_interface.h"
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
		constexpr const char kWrongRelease[]   = "The downloaded file is not the one the mod's latest release describes, so nothing was installed.";
		constexpr const char kRefusedRelease[] = "The mod's latest release points somewhere the mod list doesn't allow, so nothing was installed.";
		constexpr const char kHugeRelease[]    = "The mod's latest release information is too large to check, so nothing was installed.";
		constexpr const char kWriteFailed[]    = "The mod's files could not be written. Nothing was installed.";
		constexpr const char kCancelled[]      = "The install was stopped.";
		constexpr const char kTempSuffix[]     = ".install.tmp";
		constexpr int        kMoveRetries      = 3;
		constexpr DWORD      kMoveRetryMs      = 200;

		using UpdateHosts::IsDownloadHost;

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

		// The pieces after the host of a github.com address of the form
		// <owner>/<repo>/releases/<a>/<b>/<name>, all plain; else none.
		std::vector<std::string> ReleaseSegments(const std::string& url)
		{
			static const std::string kStart = "https://github.com/";
			if (url.size() > ModIndex::kMaxUrlLen || url.compare(0, kStart.size(), kStart) != 0)
				return {};
			std::vector<std::string> p = ModIndex::detail::SplitSegments(url.substr(kStart.size()));
			if (p.size() != 6 || p[2] != "releases")
				return {};
			for (const std::string& part : p)
				if (!ModIndex::detail::IsPlainSegment(part))
					return {};
			return p;
		}

		bool SameRepo(const std::vector<std::string>& a, const std::vector<std::string>& b)
		{
			return ModIndex::detail::EqualsNoCase(a[0], b[0]) && ModIndex::detail::EqualsNoCase(a[1], b[1]);
		}

		// .../releases/latest/download/<x>-client-manifest.json of the repository
		// the list's download is in.
		bool IsOwnLatestManifest(const std::string& manifestUrl, const std::string& listedUrl)
		{
			const std::vector<std::string> manifest = ReleaseSegments(manifestUrl);
			const std::vector<std::string> listed = ReleaseSegments(listedUrl);
			return !manifest.empty() && !listed.empty() && SameRepo(manifest, listed)
				&& manifest[3] == "latest" && manifest[4] == "download"
				&& ModIndex::detail::EndsWithNoCase(manifest[5], "-client-manifest.json");
		}

		// GitHub answers .../releases/latest/download/<name> by naming the tag:
		// .../releases/download/<tag>/<name>, in the same repository.
		bool IsLatestTagStep(const std::string& latest, const std::string& to)
		{
			const std::vector<std::string> from = ReleaseSegments(latest);
			const std::vector<std::string> next = ReleaseSegments(to);
			return !from.empty() && !next.empty() && SameRepo(from, next)
				&& next[3] == "download" && next[5] == from[5];
		}

		// The list's own file name, in a release of the list's repository.
		bool IsSameAsset(const std::string& url, const std::string& listedUrl)
		{
			if (!ModIndex::IsReleaseAssetUrl(url))
				return false;
			const std::vector<std::string> offered = ReleaseSegments(url);
			const std::vector<std::string> listed = ReleaseSegments(listedUrl);
			return !offered.empty() && !listed.empty() && SameRepo(offered, listed)
				&& ModIndex::detail::EqualsNoCase(offered[5], listed[5]);
		}

		// A manifest's version is untrusted text that ends up in update_state.ini.
		bool IsPlainVersion(const std::string& version)
		{
			if (version.empty() || version.size() > ModIndex::kMaxVersionLen)
				return false;
			for (const char c : version)
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
					|| c == '.' || c == '-' || c == '_' || c == '+'))
					return false;
			return true;
		}

		// GETs `first` within maxBytes and what is left of kDeadlineMs since
		// `started`, checking every redirect. latestManifest: `first` is a
		// releases/latest/download address, whose first redirect must be the tag
		// step and nothing else. Later ones follow the same rules as any.
		bool Fetch(const std::string& first, size_t maxBytes, bool latestManifest, const std::string& label,
			const HopFn& hop, const std::atomic<bool>& cancel, ULONGLONG started, std::string& body, std::string& why)
		{
			std::string url = first;
			for (int redirects = 0;; ++redirects)
			{
				const ULONGLONG spent = GetTickCount64() - started;
				if (cancel.load() || spent >= kDeadlineMs)
				{
					why = cancel.load() ? kCancelled : kFailed;
					return false;
				}
				Hop answer = hop(url, maxBytes, kDeadlineMs - static_cast<unsigned>(spent), cancel);
				if (cancel.load())
				{
					why = kCancelled;
					return false;
				}
				if (latestManifest && answer.overLimit)
				{
					why = kHugeRelease;
					return false;
				}
				if (answer.status == 200)
				{
					body = std::move(answer.body);
					return true;
				}
				const unsigned s = answer.status;
				if (s != 301 && s != 302 && s != 303 && s != 307 && s != 308)
				{
					why = kFailed;
					return false;
				}
				const bool allowed = latestManifest && redirects == 0
					? IsLatestTagStep(url, answer.location)
					: IsAllowedRedirect(url, answer.location);
				if (redirects >= kMaxRedirects || !allowed)
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
			options.overLimit = &hop.overLimit;
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
		const std::atomic<bool>& cancel, std::string& bytes, std::string& why, unsigned long long startedMs)
	{
		bytes.clear();
		const size_t limit = static_cast<size_t>(ModIndex::kMaxDownloadBytes);
		if (!ModIndex::IsReleaseAssetUrl(info.url) || (info.hashPinned && (info.size == 0 || info.size > limit)))
		{
			why = kRefusedAddress;
			return false;
		}
		const char* const wrong = info.hashPinned ? kWrongFile : kWrongRelease;

		if (!Fetch(info.url, info.hashPinned ? info.size : limit, false, label, hop, cancel,
			startedMs ? startedMs : GetTickCount64(), bytes, why))
		{
			bytes.clear();
			return false;
		}

		if (info.hashPinned ? bytes.size() != info.size : bytes.empty() || bytes.size() > limit)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: got %zu bytes, expected %hs %zu; nothing written",
				label.c_str(), bytes.size(), info.hashPinned ? "exactly" : "1 to",
				info.hashPinned ? info.size : limit);
			// No body: the request itself failed.
			why = bytes.empty() ? kFailed : wrong;
			bytes.clear();
			return false;
		}
		if (!info.hashPinned && bytes.compare(0, 2, "MZ") != 0)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: the latest release's file is not a DLL; nothing written", label.c_str());
			bytes.clear();
			why = wrong;
			return false;
		}
		const std::string digest = Sha256::OfBytes(bytes);
		const bool compare = info.hashPinned || !info.sha256.empty();
		if (digest.empty() || (compare && digest != info.sha256))
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: SHA-256 mismatch: expected %hs, got %hs; nothing written",
				label.c_str(), info.sha256.c_str(), digest.c_str());
			bytes.clear();
			why = wrong;
			return false;
		}
		ModLoaderLogger::LogInfo(L"[Install] %hs: SHA-256 %hs (%hs)", label.c_str(), digest.c_str(),
			info.hashPinned ? "matches the list" : compare ? "latest release, matches its manifest" : "latest release, not pinned");
		return true;
	}

	Pick ResolveLatest(const Request& request, const HopFn& hop, const std::atomic<bool>& cancel,
		unsigned long long startedMs, ModIndex::InstallInfo& out, std::string& version, std::string& why)
	{
		out = request.info;
		version = request.version;
		why.clear();
		const char* const name = request.name.c_str();
		const char* const listed = request.version.c_str();
		if (request.info.manifestUrl.empty())
		{
			ModLoaderLogger::LogInfo(L"[Install] %hs: no release manifest, installing the list's %hs", name, listed);
			return Pick::Pinned;
		}
		const auto failed = [&](const char* reason)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: latest release check failed (%hs), installing the list's %hs",
				name, reason, listed);
			return Pick::Pinned;
		};

		const auto refused = [&](const char* reason, const char* words)
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: latest release refused (%hs); nothing installed", name, reason);
			why = words;
			return Pick::Refuse;
		};

		ModLoaderLogger::LogInfo(L"[Install] %hs: checking latest release", name);
		if (!IsOwnLatestManifest(request.info.manifestUrl, request.info.url))
			return failed("the manifest address is not the repository's latest release");
		std::string manifest, fetchWhy;
		if (!Fetch(request.info.manifestUrl, kMaxManifestBytes, true, request.name, hop, cancel, startedMs,
			manifest, fetchWhy))
		{
			if (fetchWhy == kRefusedAddress)
				return refused("the manifest was sent to an address that is not allowed", kRefusedRelease);
			if (fetchWhy == kHugeRelease)
				return refused("the manifest is too large", kHugeRelease);
			return failed(fetchWhy.c_str());
		}
		if (manifest.size() > kMaxManifestBytes)
			return refused("the manifest is too large", kHugeRelease);

		// Read as the auto-update reads them, so what is checked here is what it
		// would fetch.
		const std::string remote = JsonExtractString(manifest, "version");
		const std::string url = JsonExtractString(manifest, "download_url");
		if (remote.empty() || url.empty())
			return failed("the manifest has no version or download_url");
		const std::string shown = ModIndex::detail::Clean(remote, ModIndex::kMaxVersionLen);

		const bool hasMin = JsonFindValue(manifest, "interface_version_min") != std::string::npos;
		const bool hasMax = JsonFindValue(manifest, "interface_version_max") != std::string::npos;
		const int ifaceMin = ModIndex::detail::ReadInt(manifest, "interface_version_min");
		const int ifaceMax = ModIndex::detail::ReadInt(manifest, "interface_version_max");
		if (ifaceMin >= 0 && ifaceMax >= 0
			&& (ifaceMax < PLUGIN_INTERFACE_VERSION_MIN || ifaceMin > PLUGIN_INTERFACE_VERSION_MAX))
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: latest %hs needs interface [%d, %d], installing the list's %hs",
				name, shown.c_str(), ifaceMin, ifaceMax, listed);
			return Pick::Pinned;
		}

		const AutoUpdateVersion::ParsedVersion offered = AutoUpdateVersion::ParseVersion(remote.c_str());
		const AutoUpdateVersion::ParsedVersion pinned = AutoUpdateVersion::ParseVersion(listed);

		// The .json written beside the DLL names this manifest. If the next launch
		// could take its release from somewhere else, nothing is installed. With
		// a list version that has no number to compare, it always could: the
		// auto-update then goes by the version the DLL reports.
		if (!IsSameAsset(url, request.info.url)
			&& (!pinned.valid || AutoUpdateVersion::ReleaseIsNewer(remote.c_str(), listed)))
		{
			ModLoaderLogger::LogWarn(L"[Install] %hs: latest %hs refused: its download address is not the list's file in the list's repository",
				name, shown.c_str());
			why = kRefusedRelease;
			return Pick::Refuse;
		}

		if (!IsPlainVersion(remote) || !offered.valid || !pinned.valid
			|| AutoUpdateVersion::CompareVersions(offered, pinned) <= 0)
		{
			ModLoaderLogger::LogInfo(L"[Install] %hs: latest %hs not newer than list %hs, installing the list's",
				name, shown.c_str(), listed);
			return Pick::Pinned;
		}
		if ((hasMin && ifaceMin < 0) || (hasMax && ifaceMax < 0))
			return failed("its interface range cannot be read");

		std::string sha;
		if (JsonFindValue(manifest, "download_sha256") != std::string::npos)
		{
			sha = JsonExtractString(manifest, "download_sha256");
			for (char& c : sha)
				c = ModIndex::detail::Lower(c);
			if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos)
				return failed("its download_sha256 is not 64 hex digits");
		}

		out.url = url;
		out.sha256 = sha;
		out.size = 0;
		out.hashPinned = false;
		version = remote;
		ModLoaderLogger::LogInfo(L"[Install] %hs: latest is %hs (list %hs), installing it", name, remote.c_str(), listed);
		return Pick::Latest;
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
			std::string bytes;
			bool downloaded = false;
			try
			{
				outcome.message = CheckTarget(request);
				if (outcome.message.empty())
				{
					// One deadline for the release check and the download. A download
					// that fails is a failed install: only the check falls back.
					const ULONGLONG started = GetTickCount64();
					ModIndex::InstallInfo info;
					std::string version, why;
					if (ResolveLatest(request, shared->hop, shared->cancel, started, info, version, why) == Pick::Refuse)
						outcome.message = why;
					else
					{
						outcome.version = version;
						ModLoaderLogger::LogInfo(L"[Install] %hs: downloading %hs", request.name.c_str(), info.url.c_str());
						downloaded = Download(info, request.name, shared->hop, shared->cancel, bytes, outcome.message, started);
					}
				}
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
					ModLoaderLogger::LogInfo(L"[Install] %hs: %hs written",
						request.name.c_str(), request.info.dllFile.c_str());
				else
					ModLoaderLogger::LogWarn(L"[Install] %hs: failed: %hs", request.name.c_str(), outcome.message.c_str());
				try { done(outcome); }
				catch (...) {}
			}
			// Still under the lock taken above, so Start and Busy never see a
			// stale value.
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
