#include "plugin_catalog.h"
#include "plugin_catalog_text.h"
#include "logging/logger.h"

#include <memory>
#include <mutex>

namespace PluginCatalog
{
	namespace
	{
		std::mutex                          g_lock;
		std::shared_ptr<const std::string>  g_text;   // null: no catalog

		void Publish(const PluginCatalogText::Catalog& catalog)
		{
			auto text = std::make_shared<const std::string>(PluginCatalogText::ToAccessorText(catalog));
			std::lock_guard<std::mutex> lock(g_lock);
			g_text = std::move(text);
		}

		// The cache file at `path` as a verified, parsed catalog with its body.
		bool ReadVerified(const std::wstring& path, const std::vector<VersionFeed::PublicKey>& keys,
			PluginCatalogText::Catalog& catalog, std::string& body, bool& missing, std::string& error)
		{
			std::string file;
			if (!VersionFeed::ReadFileCapped(path, file, missing))
			{
				error = missing ? "no cache file" : "the cache file could not be read";
				return false;
			}
			VersionFeed::VerifiedBody verified;
			if (!VersionFeed::VerifySigned(file, keys, verified, error)
				|| !PluginCatalogText::Parse(verified.body, catalog, error))
				return false;
			body = std::move(verified.body);
			return true;
		}
	}

	Outcome Store(const std::wstring& path, const std::string& file,
		const std::vector<VersionFeed::PublicKey>& keys, std::string& error)
	{
		VersionFeed::VerifiedBody incoming;
		PluginCatalogText::Catalog catalog;
		if (!VersionFeed::VerifySigned(file, keys, incoming, error)
			|| !PluginCatalogText::Parse(incoming.body, catalog, error))
			return Outcome::Rejected;

		PluginCatalogText::Catalog cachedCatalog;
		PluginCatalogText::Cached  cached;
		bool missing = false;
		std::string ignored;
		const bool haveCache = ReadVerified(path, keys, cachedCatalog, cached.body, missing, ignored);
		cached.seq = cachedCatalog.seq;

		switch (PluginCatalogText::DecideCache(haveCache ? &cached : nullptr, catalog.seq, incoming.body))
		{
		case PluginCatalogText::CacheDecision::Unchanged:
			return Outcome::Unchanged;
		case PluginCatalogText::CacheDecision::RefuseRollback:
			error = "seq " + std::to_string(catalog.seq) + " is below the cache's " + std::to_string(cached.seq);
			return Outcome::RefusedRollback;
		case PluginCatalogText::CacheDecision::RefuseConflict:
			error = "seq " + std::to_string(catalog.seq) + " is the cache's, with other contents";
			return Outcome::RefusedConflict;
		case PluginCatalogText::CacheDecision::Replace:
			break;
		}
		if (!VersionFeed::WriteFileAtomic(path, file, error))
			return Outcome::Rejected;
		Publish(catalog);
		return Outcome::Stored;
	}

	void LoadCache(const std::wstring& path, const std::vector<VersionFeed::PublicKey>& keys)
	{
		if (keys.empty())
			return;
		PluginCatalogText::Catalog catalog;
		std::string body, error;
		bool missing = false;
		if (!ReadVerified(path, keys, catalog, body, missing, error))
		{
			if (!missing)
				ModLoaderLogger::LogWarn(L"[PluginCatalog] The cached plugin catalog is not usable (%hs)", error.c_str());
			return;
		}
		Publish(catalog);
		ModLoaderLogger::LogInfo(L"[PluginCatalog] Plugin catalog seq %llu loaded from the cache (%zu plugins, %zu features)",
			static_cast<unsigned long long>(catalog.seq), catalog.plugins.size(), catalog.features.size());
	}

	std::string AccessorText()
	{
		std::shared_ptr<const std::string> text;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			text = g_text;
		}
		return text ? *text : std::string("unavailable");
	}
}
