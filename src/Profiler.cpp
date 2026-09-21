// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "Clipmap.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>

namespace Profiler
{
	namespace
	{

		constexpr uint32_t kFrameLatency = 4;

		constexpr uint32_t kMaxScopes = 256;

		constexpr uint32_t kMaxSamples = 512;

		constexpr float kMaxPlausibleFrameMs = 250.0f;

		struct ScopePair
		{
			ID3D11Query* begin{ nullptr };
			ID3D11Query* end{ nullptr };
			Scope        which{ Scope::kCount };
			bool         closed{ false };
		};

		struct Snapshot
		{
			uint32_t counts[static_cast<size_t>(Count::kCount)]{};
			int64_t  cpuTicks[static_cast<size_t>(CpuScope::kCount)]{};
		};

		struct FrameSlot
		{

			ID3D11Query* disjoint{ nullptr };
			ID3D11Query* spanBegin{ nullptr };
			ID3D11Query* spanEnd{ nullptr };
			ID3D11Query* stats{ nullptr };

			ID3D11Query* clipBegin{ nullptr };
			ID3D11Query* clipEnd{ nullptr };
			bool         clipClosed{ false };

			ScopePair scopes[kMaxScopes]{};
			uint32_t  used{ 0 };
			uint32_t  dropped{ 0 };
			bool      scopesEnabled{ false };

			Snapshot snapshot{};
			bool     open{ false };
			bool     inFlight{ false };
		};

		FrameSlot g_frames[kFrameLatency]{};
		uint32_t  g_write{ 0 };
		uint32_t  g_read{ 0 };
		bool      g_initialized{ false };
		bool      g_scopesCreated{ false };

		enum class OpenKind
		{
			kNone,
			kClipmap,
			kPooled
		};
		OpenKind g_openKind{ OpenKind::kNone };
		uint32_t g_openIndex{ 0 };

		std::atomic<uint32_t> g_live[static_cast<size_t>(Count::kCount)]{};
		std::atomic<int64_t>  g_liveCpuTicks[static_cast<size_t>(CpuScope::kCount)]{};

		double g_qpcToMs{ 0.0 };

		struct Accum
		{
			double   sum{ 0.0 };
			float    peak{ 0.0f };
			float    low{ 0.0f };
			uint32_t n{ 0 };
			float    samples[kMaxSamples]{};
			uint32_t stored{ 0 };

			void Push(float a_ms)
			{
				if (n == 0 || a_ms < low) {
					low = a_ms;
				}
				sum += a_ms;
				++n;
				if (a_ms > peak) {
					peak = a_ms;
				}
				if (stored < kMaxSamples) {
					samples[stored++] = a_ms;
				}
			}

			float Avg() const
			{
				return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
			}

			float P95() const
			{
				if (stored == 0) {
					return 0.0f;
				}
				static float scratch[kMaxSamples];
				std::copy_n(samples, stored, scratch);
				const auto k = static_cast<uint32_t>(0.95f * static_cast<float>(stored - 1));
				std::nth_element(scratch, scratch + k, scratch + stored);
				return scratch[k];
			}
		};

		Accum g_frameMs{};
		Accum g_clipMs{};
		Accum g_landMs{};
		Accum g_landDepthMs{};
		Accum g_actorMs{};
		Accum g_cpuMs[static_cast<size_t>(CpuScope::kCount)]{};

		uint64_t g_hsInvocations{ 0 };
		uint64_t g_dsInvocations{ 0 };
		uint64_t g_rasterPrims{ 0 };
		double   g_countSums[static_cast<size_t>(Count::kCount)]{};

		uint32_t g_scopeFrames{ 0 };
		uint32_t g_scopeDrops{ 0 };
		uint32_t g_disjointFrames{ 0 };
		uint32_t g_hitchFrames{ 0 };
		float    g_reportTimer{ 0.0f };

		uint32_t g_refreshHz{ 0 };

		bool g_windowFollowsReload{ false };

		void ClearAccumulators()
		{
			g_frameMs = Accum{};
			g_clipMs = Accum{};
			g_landMs = Accum{};
			g_landDepthMs = Accum{};
			g_actorMs = Accum{};
			for (auto& accum : g_cpuMs) {
				accum = Accum{};
			}

			g_hsInvocations = 0;
			g_dsInvocations = 0;
			g_rasterPrims = 0;
			for (auto& sum : g_countSums) {
				sum = 0.0;
			}

			g_scopeFrames = 0;
			g_scopeDrops = 0;
			g_disjointFrames = 0;
			g_hitchFrames = 0;
		}

		bool CreateScopeQueries()
		{
			auto* device = globals::d3d::device;
			if (!device) {
				return false;
			}

			const D3D11_QUERY_DESC ts{ D3D11_QUERY_TIMESTAMP, 0 };

			for (auto& frame : g_frames) {
				for (auto& scope : frame.scopes) {
					if (scope.begin) {
						continue;
					}
					if (FAILED(device->CreateQuery(&ts, &scope.begin)) ||
						FAILED(device->CreateQuery(&ts, &scope.end))) {
						logger::error("Profiler: could not create the per-draw timestamp "
									  "queries; draw scopes stay off");
						return false;
					}
				}
			}

			logger::info("Profiler: per-draw scopes on, {} brackets a frame across {} frames "
						 "in flight",
				kMaxScopes, kFrameLatency);
			return true;
		}

		uint32_t DisplayRefreshHz()
		{
			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);

			if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {

				return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
			}
			return 0;
		}

		bool Initialize()
		{
			auto* device = globals::d3d::device;
			if (!device) {
				return false;
			}

			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			g_qpcToMs = frequency.QuadPart ?
							1000.0 / static_cast<double>(frequency.QuadPart) :
							0.0;

			const D3D11_QUERY_DESC ts{ D3D11_QUERY_TIMESTAMP, 0 };
			const D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			const D3D11_QUERY_DESC stats{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };

			for (auto& frame : g_frames) {
				if (FAILED(device->CreateQuery(&disjoint, &frame.disjoint)) ||
					FAILED(device->CreateQuery(&ts, &frame.spanBegin)) ||
					FAILED(device->CreateQuery(&ts, &frame.spanEnd)) ||
					FAILED(device->CreateQuery(&ts, &frame.clipBegin)) ||
					FAILED(device->CreateQuery(&ts, &frame.clipEnd)) ||
					FAILED(device->CreateQuery(&stats, &frame.stats))) {
					logger::error("Profiler: query creation failed - staying off");
					Release();
					return false;
				}
			}

			g_write = 0;
			g_read = 0;
			g_openKind = OpenKind::kNone;
			g_reportTimer = 0.0f;
			ClearAccumulators();

			g_refreshHz = DisplayRefreshHz();
			if (g_refreshHz == 0) {
				logger::info("Profiler: could not read the display refresh rate, so it "
							 "cannot tell you when the frame span is a limiter rather than "
							 "GPU work. Compare the fps it prints against your own refresh "
							 "rate.");
			}

			g_initialized = true;
			logger::info("Profiler: on, reporting every {:.1f}s. The frame span runs "
						 "Main_RenderDepth to Main_RenderDepth, so an A/B on "
						 "EnableTessellation measures the whole cost of it.",
				Settings::profileInterval);
			return true;
		}

		Snapshot TakeSnapshot()
		{
			Snapshot out{};
			for (size_t i = 0; i < static_cast<size_t>(Count::kCount); ++i) {
				out.counts[i] = g_live[i].load(std::memory_order_relaxed);
			}
			for (size_t i = 0; i < static_cast<size_t>(CpuScope::kCount); ++i) {
				out.cpuTicks[i] = g_liveCpuTicks[i].load(std::memory_order_relaxed);
			}
			return out;
		}

		void ZeroLiveCounters()
		{
			for (auto& counter : g_live) {
				counter.store(0, std::memory_order_relaxed);
			}
			for (auto& ticks : g_liveCpuTicks) {
				ticks.store(0, std::memory_order_relaxed);
			}
		}

		void CloseSpan(ID3D11DeviceContext* a_context)
		{
			auto& frame = g_frames[g_write];
			if (!frame.open) {
				return;
			}

			if (g_openKind != OpenKind::kNone) {
				GpuEnd();
			}

			a_context->End(frame.spanEnd);
			a_context->End(frame.stats);
			a_context->End(frame.disjoint);

			frame.snapshot = TakeSnapshot();
			frame.open = false;
			frame.inFlight = true;

			g_write = (g_write + 1) % kFrameLatency;
		}

		bool TryCollect(ID3D11DeviceContext* a_context, FrameSlot& a_frame)
		{

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (a_context->GetData(a_frame.disjoint, &disjoint, sizeof(disjoint),
					D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
				return false;
			}

			if (disjoint.Disjoint || disjoint.Frequency == 0) {
				++g_disjointFrames;
				return true;
			}

			const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);

			UINT64 spanBegin = 0;
			UINT64 spanEnd = 0;
			if (a_context->GetData(a_frame.spanBegin, &spanBegin, sizeof(spanBegin), 0) != S_OK ||
				a_context->GetData(a_frame.spanEnd, &spanEnd, sizeof(spanEnd), 0) != S_OK) {
				return false;
			}
			const auto spanMs =
				static_cast<float>(static_cast<double>(spanEnd - spanBegin) * toMs);

			if (spanMs > kMaxPlausibleFrameMs) {
				++g_hitchFrames;
				return true;
			}

			g_frameMs.Push(spanMs);

			if (a_frame.clipClosed) {
				UINT64 begin = 0;
				UINT64 end = 0;
				if (a_context->GetData(a_frame.clipBegin, &begin, sizeof(begin), 0) == S_OK &&
					a_context->GetData(a_frame.clipEnd, &end, sizeof(end), 0) == S_OK) {
					g_clipMs.Push(static_cast<float>(static_cast<double>(end - begin) * toMs));
				}
			}

			D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
			if (a_context->GetData(a_frame.stats, &stats, sizeof(stats), 0) == S_OK) {

				g_hsInvocations += stats.HSInvocations;
				g_dsInvocations += stats.DSInvocations;
				g_rasterPrims += stats.CPrimitives;
			}

			if (a_frame.scopesEnabled) {
				double land = 0.0;
				double landDepth = 0.0;
				double actor = 0.0;

				for (uint32_t i = 0; i < a_frame.used; ++i) {
					const auto& scope = a_frame.scopes[i];
					if (!scope.closed) {
						continue;
					}

					UINT64 begin = 0;
					UINT64 end = 0;
					if (a_context->GetData(scope.begin, &begin, sizeof(begin), 0) != S_OK ||
						a_context->GetData(scope.end, &end, sizeof(end), 0) != S_OK) {
						continue;
					}

					const double ms = static_cast<double>(end - begin) * toMs;
					if (scope.which == Scope::kLandscape) {
						land += ms;
					} else if (scope.which == Scope::kLandscapeDepth) {
						landDepth += ms;
					} else if (scope.which == Scope::kOtherRouted) {
						actor += ms;
					}
				}

				g_landMs.Push(static_cast<float>(land));
				g_landDepthMs.Push(static_cast<float>(landDepth));
				g_actorMs.Push(static_cast<float>(actor));
				g_scopeDrops += a_frame.dropped;
				++g_scopeFrames;
			}

			for (size_t i = 0; i < static_cast<size_t>(Count::kCount); ++i) {
				g_countSums[i] += static_cast<double>(a_frame.snapshot.counts[i]);
			}
			for (size_t i = 0; i < static_cast<size_t>(CpuScope::kCount); ++i) {
				g_cpuMs[i].Push(static_cast<float>(
					static_cast<double>(a_frame.snapshot.cpuTicks[i]) * g_qpcToMs));
			}

			return true;
		}

		void CollectReady(ID3D11DeviceContext* a_context)
		{
			while (true) {
				auto& frame = g_frames[g_read];
				if (!frame.inFlight || !TryCollect(a_context, frame)) {
					return;
				}
				frame.inFlight = false;
				g_read = (g_read + 1) % kFrameLatency;
			}
		}

		void OpenSpan(ID3D11DeviceContext* a_context)
		{
			auto& frame = g_frames[g_write];

			if (frame.inFlight) {
				return;
			}

			frame.used = 0;
			frame.dropped = 0;
			frame.clipClosed = false;
			frame.scopesEnabled = Settings::profileDrawScopes && g_scopesCreated;

			a_context->Begin(frame.disjoint);
			a_context->Begin(frame.stats);
			a_context->End(frame.spanBegin);

			frame.open = true;
		}

		float Share(float a_part, float a_whole)
		{
			return a_whole > 0.0f ? 100.0f * a_part / a_whole : 0.0f;
		}

		void Report()
		{
			if (g_frameMs.n == 0) {
				logger::info("Profile: no frames collected - the GPU has not returned a "
							 "result yet, or nothing is rendering");
				return;
			}

			const auto  frames = g_frameMs.n;
			const float perFrame = 1.0f / static_cast<float>(frames);
			const float frameAvg = g_frameMs.Avg();

			logger::info("--- Profile: {} frames | clipmap {}^2 @ {:.2f} wu/cell | spacing "
						 "{:.1f} maxfactor {:.0f} | tess={} depth={} paint={} scopes={} ---",
				frames, Clipmap::kTexels, Clipmap::kCellSize,
				Settings::tessellationTargetSpacing, Settings::tessellationMaxFactor,
				Settings::enableTessellation, Settings::enableDepthPass,
				Settings::enableActorPaint, Settings::profileDrawScopes);

			const float fps = frameAvg > 0.0f ? 1000.0f / frameAvg : 0.0f;

			logger::info("  GPU frame span    avg {:7.3f} ms ({:5.1f} fps)  p95 {:7.3f}  "
						 "max {:7.3f}  min {:7.3f}",
				frameAvg, fps, g_frameMs.P95(), g_frameMs.peak, g_frameMs.low);

			if (g_refreshHz >= 20 && fps > 0.0f) {
				for (uint32_t interval = 1; interval <= 4; ++interval) {
					const float target =
						static_cast<float>(g_refreshHz) / static_cast<float>(interval);

					if (std::fabs(fps - target) <= 0.02f * target) {
						logger::warn("    ^ {:.1f} fps is your {} Hz display refresh rate "
									 "(vsync interval {}), so that span is the frame INTERVAL, "
									 "not GPU work. An A/B on it will show nothing. Uncap the "
									 "framerate, or push the GPU until fps drops below {}. "
									 "Every line below this one is measured INSIDE the frame "
									 "and is unaffected.",
							fps, g_refreshHz, interval, target);
						break;
					}
				}
			}

			if (g_clipMs.n > 0) {
				const float clipAvg = g_clipMs.Avg();
				logger::info("  Clipmap dispatch  avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
							 "({:.1f}% of the frame)",
					clipAvg, g_clipMs.P95(), g_clipMs.peak, Share(clipAvg, frameAvg));

				logger::info("    -> at {} texels ({:.2f} wu/cell) expect about {:.3f} ms, "
							 "{:.1f}% of this frame",
					Clipmap::kTexels * 2, Clipmap::kCellSize * 0.5f, clipAvg * 4.0f,
					Share(clipAvg * 4.0f, frameAvg));
			}

			if (g_scopeFrames > 0) {

				logger::info("  Landscape colour  avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
							 "(sum of per-draw scopes; overlap makes it an upper bound)",
					g_landMs.Avg(), g_landMs.P95(), g_landMs.peak);
				logger::info("  Landscape depth   avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}",
					g_landDepthMs.Avg(), g_landDepthMs.P95(), g_landDepthMs.peak);
				logger::info("  Other routed      avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
							 "(actor paint, and the probe and mesh raise when on)",
					g_actorMs.Avg(), g_actorMs.P95(), g_actorMs.peak);

				if (g_scopeDrops > 0) {
					logger::warn("    {} brackets past the {}-a-frame scope pool went "
								 "untimed, so the two lines above are low",
						g_scopeDrops, kMaxScopes);
				}
			}

			const auto& bracket = g_cpuMs[static_cast<size_t>(CpuScope::kDrawBracket)];
			const auto& gather = g_cpuMs[static_cast<size_t>(CpuScope::kGatherStamps)];

			logger::info("  Bracket CPU       avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
						 "(BeginDraw + EndDraw, summed over the frame)",
				bracket.Avg(), bracket.P95(), bracket.peak);
			logger::info("  Gather CPU        avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
						 "(actors, collision shapes and the land under them)",
				gather.Avg(), gather.P95(), gather.peak);

			const auto& coverage = g_cpuMs[static_cast<size_t>(CpuScope::kSnowCoverage)];
			logger::info("  Coverage CPU      avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
						 "(land material queries for the snow raise)",
				coverage.Avg(), coverage.P95(), coverage.peak);

			const auto& shelter = g_cpuMs[static_cast<size_t>(CpuScope::kShelter)];
			logger::info("  Shelter CPU       avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}   "
						 "(upward rays asking what has a roof over it)",
				shelter.Avg(), shelter.P95(), shelter.peak);

			for (const auto& [scope, name] : {
				std::pair{ CpuScope::kObjectScan, "Object scan CPU" },
				std::pair{ CpuScope::kShaderPrepare, "Shader prepare CPU" },
				std::pair{ CpuScope::kMagicImpacts, "Magic impacts CPU" } }) {
				const auto& sample = g_cpuMs[static_cast<size_t>(scope)];
				logger::info("  {:18} avg {:7.3f} ms  p95 {:7.3f}  max {:7.3f}",
					name, sample.Avg(), sample.P95(), sample.peak);
			}

			logger::info("  Geometry          HS {:9.0f}  DS {:9.0f} invocations/frame | "
						 "{:9.0f} primitives rasterised/frame",
				static_cast<double>(g_hsInvocations) * perFrame,
				static_cast<double>(g_dsInvocations) * perFrame,
				static_cast<double>(g_rasterPrims) * perFrame);

			const auto count = [&](Count a_which) {
				return static_cast<float>(g_countSums[static_cast<size_t>(a_which)]) * perFrame;
			};

			const float colourRouted = count(Count::kLandscapeRouted);
			const float depthRouted = count(Count::kDepthRouted);

			logger::info("  Routing colour    seen {:5.1f}  culled {:5.1f}  routed {:5.1f} "
						 "draws/frame",
				count(Count::kLandscapeSeen), count(Count::kLandscapeCulled), colourRouted);
			logger::info("  Routing depth     seen {:5.1f}  culled {:5.1f}  routed {:5.1f} "
						 "draws/frame  (+{:.1f} shadow-map draws left vanilla)",
				count(Count::kDepthSeen), count(Count::kDepthCulled), depthRouted,
				count(Count::kDepthShadowSkipped));
			logger::info("  Routing actors    {:5.1f} draws/frame painted",
				count(Count::kActorRouted));

			const double staticSeen = count(Count::kStaticSeen);
			if (staticSeen > 0.0) {
				logger::info("  Trap 8 probe      seen {:5.1f}  routed {:5.1f} static draws/frame",
					staticSeen, count(Count::kStaticRouted));
			}

			if (colourRouted > 0.0f && depthRouted > 1.5f * colourRouted) {
				logger::warn("    ^ the depth pass is routing {:.1f}x the draws the colour "
							 "pass is. Both call ShouldRoute on the same geometry, so the "
							 "seen and culled counts above say which half differs.",
					depthRouted / colourRouted);
			}

			const float builds = count(Count::kShaderBuilds);
			if (builds > 0.0f) {

				if (g_windowFollowsReload) {
					logger::info("  Shader builds     {:.2f}/frame - expected, the reload that "
								 "started this window dropped the cache. The compile is also "
								 "what the bracket CPU maximum above is.",
						builds);
				} else {
					logger::warn("  Shader builds     {:.2f}/frame - a steady state should be "
								 "0; something is dropping the cache",
						builds);
				}
			}

			if (g_hitchFrames > 0) {
				logger::info("  {} frames over {:.0f} ms discarded as hitches rather than "
							 "frames - an alt-tab is the usual cause and one of them would "
							 "otherwise dominate every average above",
					g_hitchFrames, kMaxPlausibleFrameMs);
			}

			if (g_disjointFrames > 0) {
				logger::warn("  {} frames discarded as disjoint (the GPU changed clock rate). "
							 "If that is most of them, the numbers above are a small sample.",
					g_disjointFrames);
			}

			if (g_frameMs.n > kMaxSamples) {
				logger::info("  p95 is over the first {} frames of the window; the averages "
							 "and extremes cover all {}",
					kMaxSamples, g_frameMs.n);
			}

			g_windowFollowsReload = false;
		}
	}

	bool Enabled()
	{
		return g_initialized;
	}

	void Frame(float a_rawDeltaSeconds)
	{
		if (!Settings::enableProfiler) {
			if (g_initialized) {
				Release();
			}
			return;
		}

		if (!globals::Ready()) {
			return;
		}

		if (!g_initialized && !Initialize()) {
			return;
		}

		if (Settings::profileDrawScopes && !g_scopesCreated) {
			g_scopesCreated = CreateScopeQueries();
		}

		auto* context = globals::d3d::context;

		CloseSpan(context);
		CollectReady(context);
		OpenSpan(context);

		ZeroLiveCounters();

		g_reportTimer += std::max(a_rawDeltaSeconds, 0.0f);
		if (g_reportTimer >= Settings::profileInterval) {
			g_reportTimer = 0.0f;
			Report();
			ClearAccumulators();
		}
	}

	void GpuBegin(Scope a_scope)
	{
		if (!g_initialized || g_openKind != OpenKind::kNone) {
			return;
		}

		auto& frame = g_frames[g_write];
		if (!frame.open) {
			return;
		}

		auto* context = globals::d3d::context;

		if (a_scope == Scope::kClipmap) {
			context->End(frame.clipBegin);
			g_openKind = OpenKind::kClipmap;
			return;
		}

		if (!frame.scopesEnabled) {
			return;
		}

		if (frame.used >= kMaxScopes) {
			++frame.dropped;
			return;
		}

		auto& scope = frame.scopes[frame.used];
		scope.which = a_scope;
		scope.closed = false;
		context->End(scope.begin);

		g_openKind = OpenKind::kPooled;
		g_openIndex = frame.used;
		++frame.used;
	}

	void GpuEnd()
	{
		if (g_openKind == OpenKind::kNone) {
			return;
		}

		auto& frame = g_frames[g_write];
		auto* context = globals::d3d::context;

		if (g_openKind == OpenKind::kClipmap) {
			context->End(frame.clipEnd);
			frame.clipClosed = true;
		} else {
			auto& scope = frame.scopes[g_openIndex];
			context->End(scope.end);
			scope.closed = true;
		}

		g_openKind = OpenKind::kNone;
	}

	void Tally(Count a_what, uint32_t a_howMany)
	{
		if (!g_initialized) {
			return;
		}
		g_live[static_cast<size_t>(a_what)].fetch_add(a_howMany, std::memory_order_relaxed);
	}

	int64_t Ticks()
	{
		if (!g_initialized) {
			return 0;
		}
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	void AddCpuTicks(CpuScope a_scope, int64_t a_ticks)
	{
		if (!g_initialized || a_ticks <= 0) {
			return;
		}
		g_liveCpuTicks[static_cast<size_t>(a_scope)].fetch_add(
			a_ticks, std::memory_order_relaxed);
	}

	void Reset()
	{
		if (!g_initialized) {
			return;
		}

		ClearAccumulators();
		g_reportTimer = 0.0f;
		g_windowFollowsReload = true;

		for (auto& frame : g_frames) {
			frame.open = false;
			frame.inFlight = false;
			frame.used = 0;
			frame.dropped = 0;
			frame.clipClosed = false;
		}
		g_write = 0;
		g_read = 0;
		g_openKind = OpenKind::kNone;
	}

	void Release()
	{
		const auto drop = [](ID3D11Query*& a_query) {
			if (a_query) {
				a_query->Release();
				a_query = nullptr;
			}
		};

		for (auto& frame : g_frames) {
			drop(frame.disjoint);
			drop(frame.spanBegin);
			drop(frame.spanEnd);
			drop(frame.clipBegin);
			drop(frame.clipEnd);
			drop(frame.stats);
			for (auto& scope : frame.scopes) {
				drop(scope.begin);
				drop(scope.end);
				scope.closed = false;
			}
			frame.used = 0;
			frame.dropped = 0;
			frame.open = false;
			frame.inFlight = false;
		}

		g_openKind = OpenKind::kNone;
		g_scopesCreated = false;

		if (g_initialized) {
			logger::info("Profiler: off");
		}
		g_initialized = false;
	}
}
