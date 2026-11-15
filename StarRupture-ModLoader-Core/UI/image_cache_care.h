#pragma once

// Housekeeping for the image library's cache folder (image_cache_file.h names its layout):
// <cache root>\<folder key>\*.img and sets.lst. Everything here touches the disk, so it runs
// on the writer's worker (image_cache_writer.h) and nowhere else.
//
//   Tidy  at start: keeps the current folder and the newest other one (a folder is as new as
//         its newest file) and deletes the rest of the root; removes the temp files of game
//         instances that are gone; then trims the kept folders to the size cap.
//   Clear deletes everything in the root (the "Clear image cache" option). Its job is also
//         taken by a writer that has gone off, and Tidy does nothing without a current folder
//         (a session whose disk part is off has none), so tidying can never delete the folder
//         in use.
//   Touch marks a file as used, which is what "least recently used" means below.
//
// Least recently used is the file's last-write time. Nothing reads the clock per frame: a
// file gets a new time when it is written, and Touch gives it one when it is used again (the
// writer calls it for an image that is already on disk, and whatever reads a file back calls
// it on a hit). The cap pass deletes the oldest *.img files first, from the older kept
// folder before the current one.
//
// A file or folder that cannot be deleted (another game instance has it open, or a scanner
// does) is skipped without a word and tried again at the next start.

#include "UI/image_cache_writer.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ImageCache::Care
{
	// The cache root's limit. Over it, the oldest files go until it fits.
	inline constexpr uint64_t kCapBytes = 256ull * 1024 * 1024;

	struct Result
	{
		uint32_t foldersRemoved   = 0;   // old folders gone entirely
		uint32_t filesRemoved     = 0;   // every file deleted, whatever the reason
		uint32_t leftoversRemoved = 0;   // the temp files of instances that are gone (also in filesRemoved)
		uint64_t bytesFreed       = 0;
	};

	// Worker only. `current` is the folder this instance writes to; it need not exist yet.
	Result Tidy(const std::wstring& root, const std::wstring& current, uint64_t capBytes = kCapBytes);

	// Worker only. Empties the root and leaves the root itself.
	Result Clear(const std::wstring& root);

	// Sets a file's last-write time to now. Worker only; a failure is ignored.
	void Touch(const std::wstring& file);

	// Is a game instance with this process id still running? An id that cannot be checked
	// counts as running.
	bool ProcessAlive(unsigned long pid);

	using Log = std::function<void(const std::string&)>;

	// Jobs for the writer's worker. `root` and `current` are called on the worker.
	Job TidyJob(std::function<std::wstring()> root, std::function<std::wstring()> current, Log log);
	Job ClearJob(std::function<std::wstring()> root, Log log);
}
