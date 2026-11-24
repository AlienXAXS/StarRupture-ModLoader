#include "mod_index_signed.h"
#include "feed_etag.h"
#include "feed_fetch.h"
#include "http_get.h"
#include "logging/logger.h"

#include <utility>

namespace ModIndex
{
	namespace
	{
		constexpr const char kUnverified[]   = "The mod list could not be verified, so it was not used.";
		constexpr const char kUnknownForm[]  = "The mod list is in a form this loader does not know, so it was not used.";
		constexpr const char kOlder[]        = "The mod list on the server is older than the one this loader already has, so it was not used.";

		// The line `text` holds from `at`, without its line feed; `next` is where the following line starts.
		std::string Line(const std::string& text, size_t at, size_t& next)
		{
			const size_t lf = text.find('\n', at);
			next = lf == std::string::npos ? text.size() : lf + 1;
			return text.substr(at, lf == std::string::npos ? std::string::npos : lf - at);
		}

		bool ReadCache(const std::wstring& path, const std::vector<VersionFeed::PublicKey>& keys, Envelope& out)
		{
			std::string file;
			bool missing = false;
			std::string error;
			return !path.empty() && VersionFeed::ReadFileCapped(path, file, missing) && ReadEnvelope(file, keys, out, error);
		}
	}

	bool IsEnvelope(const std::string& file)
	{
		return file.compare(0, 7, "format\t") == 0;
	}

	bool ReadEnvelope(const std::string& file, const std::vector<VersionFeed::PublicKey>& keys, Envelope& out, std::string& error)
	{
		out = Envelope{};
		VersionFeed::VerifiedBody verified;
		std::string why;
		if (!VersionFeed::VerifySigned(file, keys, verified, why))
		{
			ModLoaderLogger::LogWarn(L"[ModIndex] The mod list is refused (%hs)", why.c_str());
			error = kUnverified;
			return false;
		}

		size_t next = 0;
		const std::string format = Line(verified.body, 0, next);
		Line(verified.body, next, next);   // the seq line, read by VerifySigned
		const std::string kind = Line(verified.body, next, next);
		if (format != "format\t1" || kind != "kind\tmod-index")
		{
			ModLoaderLogger::LogWarn(L"[ModIndex] The mod list is refused (it is not format 1, kind mod-index)");
			error = kUnknownForm;
			return false;
		}

		out.seq  = verified.seq;
		out.body = verified.body.substr(next);
		return true;
	}

	namespace
	{
		// The loader's own address: the cache, a conditional GET, the seq rule.
		std::string FetchSigned(const Source& source, const std::string& url, std::string& error)
		{
			Envelope cache;
			const bool haveCache = ReadCache(source.cachePath, source.keys, cache);

			std::string etag;
			bool notModified = false;
			HttpGetOptions options;
			options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = FeedFetch::kOperationMs;
			options.deadlineMs = FeedFetch::kDeadlineMs;
			options.maxBytes = VersionFeed::kMaxFileBytes;
			options.allowPlainHttp = source.allowPlainHttp;
			options.ifNoneMatch = haveCache ? FeedFetch::ReadEtag(source.cachePath, cache.seq) : std::string();
			options.etag = &etag;
			options.notModified = &notModified;

			const std::string file = HttpGet(url.c_str(), "mod index", options);
			if (notModified && haveCache)
				return cache.body;
			if (file.empty())
			{
				// No answer: the last verified list is still the right one to show.
				if (haveCache)
				{
					ModLoaderLogger::LogInfo(L"[ModIndex] The mod list could not be fetched; showing the saved one (seq %llu)",
						static_cast<unsigned long long>(cache.seq));
					return cache.body;
				}
				return {};
			}

			Envelope fetched;
			if (!ReadEnvelope(file, source.keys, fetched, error))
				return {};
			if (haveCache && fetched.seq < cache.seq)
			{
				ModLoaderLogger::LogWarn(L"[ModIndex] The fetched mod list (seq %llu) is older than the saved one (seq %llu)",
					static_cast<unsigned long long>(fetched.seq), static_cast<unsigned long long>(cache.seq));
				error = kOlder;
				return {};
			}
			if (haveCache && fetched.seq == cache.seq)
			{
				if (!etag.empty())
					FeedFetch::WriteEtag(source.cachePath, cache.seq, etag);
				return cache.body;
			}

			std::string writeError;
			if (VersionFeed::WriteFileAtomic(source.cachePath, file, writeError))
			{
				if (!etag.empty())
					FeedFetch::WriteEtag(source.cachePath, fetched.seq, etag);
			}
			else
			{
				ModLoaderLogger::LogInfo(L"[ModIndex] The mod list is not saved (%hs)", writeError.c_str());
			}
			return fetched.body;
		}

		// The player's own address: the envelope or a bare index, nothing kept.
		std::string FetchAsServed(const Source& source, const std::string& url, std::string& error)
		{
			HttpGetOptions options;
			options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = FeedFetch::kOperationMs;
			options.deadlineMs = FeedFetch::kDeadlineMs;
			options.maxBytes = VersionFeed::kMaxFileBytes;
			options.allowPlainHttp = source.allowPlainHttp;

			const std::string file = HttpGet(url.c_str(), "mod index", options);
			if (!IsEnvelope(file))
				return file;
			Envelope envelope;
			return ReadEnvelope(file, source.keys, envelope, error) ? envelope.body : std::string();
		}
	}

	FetchFn MakeFetch(Source source)
	{
		return [source = std::move(source)](const std::string& url, std::string& error)
		{
			return source.signedOnly ? FetchSigned(source, url, error) : FetchAsServed(source, url, error);
		};
	}
}
