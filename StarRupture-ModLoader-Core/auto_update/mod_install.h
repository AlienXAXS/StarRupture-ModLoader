#pragma once

#include "auto_update/mod_index.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// mod_install.h -- downloads one plugin of the mod index (mod_index.h) and puts
// it in ModLoader\Plugins. It installs code, so the index, the mod's release
// manifest and every redirect are untrusted: only a GitHub release asset of the
// repository the list names, only to GitHub's own download hosts. The list's
// own file is written only if its size and SHA-256 match. A newer release (see
// ResolveLatest) has no hash in the list. No file that is already there is
// replaced. Loading the DLL is the caller's job.
//
// The network is passed in (HopFn), so a test uses it without one.
// ---------------------------------------------------------------------------

namespace ModInstall
{
	constexpr unsigned kDeadlineMs   = 120000;   // the whole download, every hop
	constexpr int      kMaxRedirects = 3;
	constexpr size_t   kMaxManifestBytes = 1024 * 1024;   // a release manifest, release notes and all
	constexpr char     kVortexManaged[] = "Vortex manages this mod. Install it in Vortex.";

	// One GET that follows no redirect. status 0: no answer. body: a 200's, whole.
	struct Hop
	{
		unsigned status = 0;
		std::string location;
		std::string body;
		bool overLimit = false;   // the body went over maxBytes, so it is not here
	};
	using HopFn = std::function<Hop(const std::string& url, size_t maxBytes, unsigned deadlineMs,
		const std::atomic<bool>& cancel)>;

	// The loader's own: HttpGet with noRedirect.
	HopFn HttpHop();

	// Whether a download at `from` may be sent on to `to`: https, no user info or
	// port; from github.com or a download host, to a download host only.
	bool IsAllowedRedirect(const std::string& from, const std::string& to);

	// The bytes of `info`. With info.hashPinned the size and SHA-256 must match.
	// Without it the file is at most kMaxDownloadBytes, starts "MZ", and matches
	// info.sha256 when that is set. On false `why` is plain words for the player.
	// `label` names the mod in the log. startedMs: the GetTickCount64 that
	// kDeadlineMs counts from; 0: now.
	bool Download(const ModIndex::InstallInfo& info, const std::string& label, const HopFn& hop,
		const std::atomic<bool>& cancel, std::string& bytes, std::string& why, unsigned long long startedMs = 0);

	struct Request
	{
		std::string name;             // the entry's name, for the log
		std::string version;          // the entry's version, handed back in the Outcome
		ModIndex::InstallInfo info;
		std::wstring pluginsDir;      // ...\ModLoader\Plugins, no trailing backslash
		std::wstring gameDir;         // the game exe's folder, no trailing backslash
	};

	enum class Pick { Pinned, Latest, Refuse };

	// What to download for `request`. Reads the release manifest at the list's
	// manifestUrl. Latest: it offers a newer version this loader can take, and
	// `out` and `version` describe it. Pinned: `out` and `version` are the list's
	// (no manifest, none that can be read, or nothing newer that fits). Refuse:
	// the manifest cannot be checked (it is over kMaxManifestBytes, or a redirect
	// to it is not allowed), or it sends a release the auto-update would take to
	// anywhere but the same file of the list's repository. The auto-update reads
	// the same manifest with neither limit, so nothing may be installed, and
	// `why` is plain words for the player. The manifest never picks the repository,
	// the file's name on disk or the .json's text.
	Pick ResolveLatest(const Request& request, const HopFn& hop, const std::atomic<bool>& cancel,
		unsigned long long startedMs, ModIndex::InstallInfo& out, std::string& version, std::string& why);

	// Why `request` may not be written, in plain words; empty: it may. Looks for
	// the files already there and for Vortex managing them.
	std::string CheckTarget(const Request& request);

	// Removes the X.json that CheckTarget accepted as a leftover of ours, but only
	// if it still holds exactly the text this install writes. Anything else is
	// left in place.
	void ClearStranded(const Request& request);

	// Writes verified bytes: temp files first, then renames that never replace a
	// file. On false nothing this call made is left behind. A test passes its own
	// `move` to change the folder just before a rename.
	bool Place(const Request& request, const std::string& bytes, std::string& why,
		const PluginManage::MoveFn& move = PluginManage::MoveOnSameVolume);

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

		// Whether Shutdown has been called. Takes no lock, so a task `done` handed
		// to another thread can ask it while this thread still holds the lock.
		bool Halted() const;

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
