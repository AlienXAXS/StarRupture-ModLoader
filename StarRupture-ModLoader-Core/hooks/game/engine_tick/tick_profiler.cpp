#include "pch.h"
#include "tick_profiler.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace Hooks::EngineTick::TickProfiler
{
	// Guards both deques below. RecordTick() (game thread) and the snapshot
	// getters (render thread) can run concurrently.
	static std::mutex s_mutex;
	static std::deque<TickSample> s_liveSamples;
	static std::deque<StutterEvent> s_stutterEvents;

	static std::string FormatTimestamp()
	{
		const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm local{};
		localtime_s(&local, &t);

		char buf[16];
		snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
		return buf;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_liveSamples.clear();
		s_stutterEvents.clear();
	}

	void RecordTick(uint64_t tickIndex, double totalMs, double avgMsAtTime, float deltaSeconds,
	                 bool isStutter, double engineMs, double dispatchMs,
	                 const std::vector<SegmentTiming>& callbackTimings,
	                 const std::vector<CallTreeNode>& callTree, size_t totalSamples)
	{
		std::lock_guard<std::mutex> lock(s_mutex);

		s_liveSamples.push_back({ tickIndex, totalMs });
		if (s_liveSamples.size() > kLiveBufferCapacity)
			s_liveSamples.pop_front();

		if (isStutter)
		{
			StutterEvent ev;
			ev.tickIndex     = tickIndex;
			ev.totalMs       = totalMs;
			ev.avgMsAtTime   = avgMsAtTime;
			ev.deltaSeconds  = deltaSeconds;
			ev.timestamp     = FormatTimestamp();
			ev.callTree      = callTree;
			ev.totalSamples  = totalSamples;

			ev.breakdown.reserve(callbackTimings.size() + 2);
			ev.breakdown.push_back({ "Engine Tick (UGameEngine::Tick)", engineMs });
			ev.breakdown.push_back({ "GameThreadDispatch::Drain", dispatchMs });
			for (const SegmentTiming& seg : callbackTimings)
				ev.breakdown.push_back(seg);

			s_stutterEvents.push_back(std::move(ev));
			if (s_stutterEvents.size() > kMaxStutterEvents)
				s_stutterEvents.pop_front();
		}
	}

	std::vector<TickSample> GetLiveSamples()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		return std::vector<TickSample>(s_liveSamples.begin(), s_liveSamples.end());
	}

	std::vector<StutterEvent> GetStutterEvents()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		return std::vector<StutterEvent>(s_stutterEvents.begin(), s_stutterEvents.end());
	}
}
