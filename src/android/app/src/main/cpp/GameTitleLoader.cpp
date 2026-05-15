#include "GameTitleLoader.h"

#include <zlib.h>

#include "config/ActiveSettings.h"

namespace
{
	// In-memory gzip inflate. WUHB icons (and many other devkitPro/wut assets)
	// are TGA payloads gzipped in the bundle. Returns nullopt on any error.
	std::optional<std::vector<uint8>> gunzip(const std::vector<uint8>& gz)
	{
		if (gz.size() < 2 || gz[0] != 0x1F || gz[1] != 0x8B)
			return std::nullopt;
		z_stream strm{};
		// 15 = max window, +32 = auto-detect gzip vs zlib header.
		if (inflateInit2(&strm, 15 + 32) != Z_OK)
			return std::nullopt;
		std::vector<uint8> out;
		out.resize(gz.size() * 4 + 256);
		strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(gz.data()));
		strm.avail_in = static_cast<uInt>(gz.size());
		strm.next_out = out.data();
		strm.avail_out = static_cast<uInt>(out.size());
		while (true)
		{
			int r = inflate(&strm, Z_NO_FLUSH);
			if (r == Z_STREAM_END)
				break;
			if (r != Z_OK)
			{
				inflateEnd(&strm);
				return std::nullopt;
			}
			if (strm.avail_out == 0)
			{
				size_t produced = out.size();
				out.resize(produced * 2);
				strm.next_out = out.data() + produced;
				strm.avail_out = static_cast<uInt>(out.size() - produced);
			}
		}
		out.resize(strm.total_out);
		inflateEnd(&strm);
		return out;
	}
}

std::optional<TitleInfo> getFirstTitleInfoByTitleId(TitleId titleId)
{
	TitleInfo titleInfo;
	if (CafeTitleList::GetFirstByTitleId(titleId, titleInfo))
		return titleInfo;
	return {};
}

GameTitleLoader::GameTitleLoader()
{
	m_loaderThread = std::thread(&GameTitleLoader::LoadGameTitles, this);
}

void GameTitleLoader::QueueTitle(TitleId titleId)
{
	{
		std::lock_guard lock(m_threadMutex);
		m_titlesToLoad.emplace_front(titleId);
	}
	m_condVar.notify_one();
}

void GameTitleLoader::SetOnTitleLoaded(std::shared_ptr<GameTitleLoadedCallback> gameTitleLoadedCallback)
{
	{
		std::lock_guard lock(m_threadMutex);
		m_gameTitleLoadedCallback = std::move(gameTitleLoadedCallback);
	}
	m_condVar.notify_one();
}

void GameTitleLoader::ReloadGameTitles()
{
	if (m_callbackIdTitleList.has_value())
	{
		CafeTitleList::UnregisterCallback(m_callbackIdTitleList.value());
	}
	m_gameInfos.clear();
	CafeTitleList::ClearScanPaths();
	for (auto&& gamePath : GetConfig().game_paths)
		CafeTitleList::AddScanPath(gamePath);
	// hb-appstore writes downloaded WUHB/RPX bundles into the emulated sd:/wiiu/apps
	// tree, which lives at <UserDataPath>/sdcard on host. Always scan it so freshly
	// installed homebrew shows up in the title list without the user having to add
	// the hidden Cemu data path to GamePaths by hand.
	CafeTitleList::AddScanPath(ActiveSettings::GetUserDataPath("sdcard/wiiu/apps"));
	CafeTitleList::Refresh();
	m_callbackIdTitleList = CafeTitleList::RegisterCallback([](CafeTitleListCallbackEvent* evt, void* ctx) { static_cast<GameTitleLoader*>(ctx)->HandleTitleListCallback(evt); }, this);
}

GameTitleLoader::~GameTitleLoader()
{
	m_continueLoading = false;
	m_condVar.notify_one();
	m_loaderThread.join();
	if (m_callbackIdTitleList.has_value())
		CafeTitleList::UnregisterCallback(m_callbackIdTitleList.value());
}

void GameTitleLoader::TitleRefresh(TitleId titleId)
{
	using namespace std::chrono;
	GameInfo2 gameInfo = CafeTitleList::GetGameInfo(titleId);
	if (!gameInfo.IsValid())
	{
		return;
	}
	TitleId baseTitleId = gameInfo.GetBaseTitleId();
	bool isNewEntry = false;
	if (auto gameInfoIt = m_gameInfos.find(baseTitleId); gameInfoIt == m_gameInfos.end())
	{
		isNewEntry = true;
		m_gameInfos[baseTitleId] = Game();
	}

	Game& game = m_gameInfos[baseTitleId];
	std::optional<TitleInfo> titleInfo = getFirstTitleInfoByTitleId(titleId);
	game.titleId = baseTitleId;
	if (titleInfo.has_value())
		game.path = titleInfo->GetPath();
	game.isFavorite = GetConfig().IsGameListFavorite(baseTitleId);
	game.name = GetNameByTitleId(baseTitleId, titleInfo);
	game.version = gameInfo.GetVersion();
	game.region = gameInfo.GetRegion();
	game.dlc = gameInfo.GetAOCVersion();
	std::shared_ptr<Image> icon = LoadIcon(baseTitleId, titleInfo);
	if (!isNewEntry)
	{
		// TOOD: update?
		return;
	}
	iosu::pdm::GameListStat playTimeStat{};
	if (iosu::pdm::GetStatForGamelist(baseTitleId, playTimeStat))
	{
		game.minutesPlayed = playTimeStat.numMinutesPlayed;
		if (playTimeStat.last_played.year != 0)
		{
			game.lastPlayed = year_month_day(year(playTimeStat.last_played.year), month(playTimeStat.last_played.month + 1), day(playTimeStat.last_played.day));
		}
	}
	if (m_gameTitleLoadedCallback)
		m_gameTitleLoadedCallback->OnTitleLoaded(game, icon);
}

void GameTitleLoader::LoadGameTitles()
{
	while (m_continueLoading)
	{
		TitleId titleId;
		{
			std::unique_lock lock(m_threadMutex);
			m_condVar.wait(lock, [this] { return (!m_titlesToLoad.empty()) || !m_continueLoading; });
			if (!m_continueLoading)
				return;
			titleId = m_titlesToLoad.front();
			m_titlesToLoad.pop_front();
		}
		TitleRefresh(titleId);
	}
}
std::string GameTitleLoader::GetNameByTitleId(TitleId titleId, const std::optional<TitleInfo>& titleInfo)
{
	auto it = m_name_cache.find(titleId);
	if (it != m_name_cache.end())
		return it->second;
	if (!titleInfo.has_value())
		return "Unknown title";
	std::string name;
	if (!GetConfig().GetGameListCustomName(titleId, name))
		name = titleInfo.value().GetMetaTitleName();
	m_name_cache.emplace(titleId, name);
	return name;
}

std::shared_ptr<Image> GameTitleLoader::LoadIcon(TitleId titleId, const std::optional<TitleInfo>& titleInfo)
{
	if (auto iconIt = m_iconCache.find(titleId); iconIt != m_iconCache.end())
		return iconIt->second;
	std::string tempMountPath = TitleInfo::GetUniqueTempMountingPath();
	if (!titleInfo.has_value())
		return {};
	auto titleInfoValue = titleInfo.value();
	if (!titleInfoValue.Mount(tempMountPath, "", FSC_PRIORITY_BASE))
		return {};
	// Retail discs / eShop titles ship meta/iconTex.tga raw. wuhbtool-built
	// homebrew ships meta/iconTex.tga.gz (gzipped TGA -- saves a few KB in the
	// bundle). A handful of homebrew uses meta/icon.png. Try in that order;
	// inflate the .gz form before handing it to the TGA decoder.
	auto iconData = fsc_extractFile((tempMountPath + "/meta/iconTex.tga").c_str());
	if (!iconData || iconData->size() <= 16)
	{
		auto gz = fsc_extractFile((tempMountPath + "/meta/iconTex.tga.gz").c_str());
		if (gz && gz->size() > 16)
			iconData = gunzip(*gz);
	}
	if (!iconData || iconData->size() <= 16)
		iconData = fsc_extractFile((tempMountPath + "/meta/icon.png").c_str());
	if (!iconData || iconData->size() <= 16)
	{
		cemuLog_log(LogType::CoreinitFile, "Failed to load icon for title {:016x}", titleId);
		titleInfoValue.Unmount(tempMountPath);
		return {};
	}
	auto image = std::make_shared<Image>(iconData.value());
	titleInfoValue.Unmount(tempMountPath);
	if (!image->IsOk())
		return {};
	m_iconCache.emplace(titleId, image);
	return image;
}

void GameTitleLoader::HandleTitleListCallback(CafeTitleListCallbackEvent* evt)
{
	if (evt->eventType == CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED || evt->eventType == CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED)
	{
		QueueTitle(evt->titleInfo->GetAppTitleId());
	}
}
