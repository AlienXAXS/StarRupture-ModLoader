#pragma once

#include "auto_update/mod_index.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// mod_install.h -- downloads one plugin of the mod index (mod_index.h) and puts
// it in ModLoader\Plugins. It installs code, so the index and every redirect
// are untrusted: only a GitHub release asset, only to GitHub's own download
// hosts, and nothing is written until the size and SHA-256 match. No file that
// is already there is replaced. Loading the DLL is the caller's job.
//
// The network is passed in (HopFn), so a test uses it without one.
// ---------------------------------------------------------------------------

namespace ModInstall
{
	constexpr unsigned kDeadlineMs   = 120000;   // the whole download, every hop
	constexpr int      kMaxRedirects = 3;

	// One GET that follows no redirect. status 0: no answer. body: a 200's, whole.
	struct Hop
	{
		unsigned status = 0;
		std::string location;
		std::string body;
	};
	using HopFn = std::function<Hop(const std::string& url, size_t maxBytes, unsigned deadlineMs,
		const std::atomic<bool>& cancel)>;

	// The loader's own: HttpGet with noRedirect.
	HopFn HttpHop();

	// Whether a download at `from` may be sent on to `to`: https, no user info or
	// port; from github.com or a download host, to a download host only.
	bool IsAllowedRedirect(const std::string& from, const std::string& to);

	// The bytes of `info`, with the size and SHA-256 checked. On false `why` is
	// plain words for the player. `label` names the mod in the log.
	bool Download(const ModIndex::InstallInfo& info, const std::string& label, const HopFn& hop,
		const std::atomic<bool>& cancel, std::string& bytes, std::string& why);

	struct Request
	{
		std::string name;             // the entry's name, for the log
		std::string version;          // the entry's version, handed back in the Outcome
		ModIndex::InstallInfo info;
		std::wstring pluginsDir;      // ...\ModLoader\Plugins, no trailing backslash
		std::wstring gameDir;         // the game exe's folder, no trailing backslash
		bool listSigned = false;      // ModIndex::Snapshot::signatureVerified
	};

	// Why `request` may not be written, in plain words; empty: it may. Looks for
	// the files already there and for Vortex managing them.
	std::string CheckTarget(const Request& request);

	// Writes verified bytes: temp files first, then renames that never replace a
	// file. On false nothing this call made is left behind.
	bool Place(const Request& request, const std::string& bytes, std::string& why);

	// A plugin an entry needs, as the caller sees it now.
	enum class Need { Met, Missing, Disabled };

	struct Requirement
	{
		std::string name;
		Need state = Need::Missing;
	};

	// Why `modName` cannot be installed yet, e.g. "MainMap needs MiniMap. Install
	// MiniMap first."; empty when every requirement is met.
	std::string RequirementMessage(const std::string& modName, const std::vector<Requirement>& needs);

	struct Outcome
	{
		bool placed = false;          // the files are in Plugins, ready to load
		std::string message;          // when not placed: plain words for the player
		std::string dllFile;
		std::string version;
	};

	// Runs on the worker thread with the installer's lock held: hand the work to
	// another thread and return. It must not call the Installer.
	using DoneFn = std::function<void(const Outcome&)>;

	// One install at a time, on its own thread.
	class Installer
	{
	public:
		explicit Installer(HopFn hop);
		~Installer();
		Installer(const Installer&) = delete;
		Installer& operator=(const Installer&) = delete;

		// False, and `done` is never called, while one is running, after
		// Shutdown, or when no thread could be started. Any thread.
		bool Start(Request request, DoneFn done);

		bool Busy() const;

		// Cancels. Once this returns nothing more is written and no `done` is
		// called. Waits up to waitMs; returns whether the worker has ended.
		bool Shutdown(unsigned waitMs);

		struct Shared;   // defined in the .cpp

	private:
		std::shared_ptr<Shared> m_shared;
	};

	// The loader's own, with HttpHop. Made on first use.
	Installer& Instance();
}
