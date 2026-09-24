// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Shelter.h"

#include "BoxFilter.h"
#include "Clipmap.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShelterTransition.h"
#include "SnowCoverage.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <vector>

namespace Shelter
{
	namespace
	{
		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		// The widest reach a refresh asks SnowCoverage about: at most 17 x 17 coverage texels.
		constexpr int32_t kMaxSnowReach = 8;

		std::vector<uint8_t> g_raw;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_cap;
		std::vector<uint8_t> g_capSmooth;
		std::vector<float> g_roofDisplay, g_capDisplay;
		std::vector<uint8_t> g_roofVisible, g_capVisible;
		// One bit per texel whose roof or cap display has not settled on its target yet. Every
		// change of g_raw or g_cap sets it (SetTarget, Reset), so the blend skips the rest.
		std::vector<uint64_t> g_fading;
		bool g_transition{ false };

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };
		bool                      g_capDirty{ false };

		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		uint32_t g_cursor{ 0 };
		uint32_t g_ageCursor{ 0 };

		// One bit per texel. Every texel that is not filled for the current window is either
		// queued (the scan still has to probe it) or a hole (its land probe failed and it waits
		// for budget the scan did not need), so the scan can jump over filled texels.
		std::vector<uint64_t> g_queued;
		std::vector<uint64_t> g_holes;
		// One bit per texel the refresh queued again. Its fill record still names its cell, so
		// while that holds, its targets come from an earlier probe of the same cell.
		std::vector<uint64_t> g_refresh;
		uint32_t              g_holeCursor{ 0 };
		int32_t               g_queuedBaseX{ 0 };
		int32_t               g_queuedBaseY{ 0 };

		int32_t  g_centreCellX{ 0 };
		int32_t  g_centreCellY{ 0 };
		bool     g_dirty{ false };
		bool     g_haveCentre{ false };
		uint32_t g_revision{ 1 };

		bool g_allocated{ false };

		void Allocate()
		{
			if (g_allocated) {
				return;
			}

			const size_t total = static_cast<size_t>(kTexels) * kTexels;
			g_raw.assign(total, 0);
			g_smooth.assign(total, 0);
			g_cap.assign(total, 255);
			g_capSmooth.assign(total, 255);
			g_roofDisplay.assign(total, 0.0f);
			g_capDisplay.assign(total, 255.0f);
			g_roofVisible.assign(total, 0);
			g_capVisible.assign(total, 255);
			g_fading.assign(total / 64, 0);
			g_rowSums.assign(total, 0);
			g_filledX.assign(total, -0x40000000);
			g_filledY.assign(total, -0x40000000);
			g_queued.assign(total / 64, ~0ull);
			g_holes.assign(total / 64, 0);
			g_refresh.assign(total / 64, 0);
			g_allocated = true;
		}

		void SetBit(std::vector<uint64_t>& a_bits, uint32_t a_index)
		{
			a_bits[a_index >> 6] |= 1ull << (a_index & 63);
		}

		void ClearBit(std::vector<uint64_t>& a_bits, uint32_t a_index)
		{
			a_bits[a_index >> 6] &= ~(1ull << (a_index & 63));
		}

		bool TestBit(const std::vector<uint64_t>& a_bits, uint32_t a_index)
		{
			return (a_bits[a_index >> 6] >> (a_index & 63)) & 1;
		}

		// Changes a texel's roof or cap target and starts its fade. Returns whether it changed.
		bool SetTarget(std::vector<uint8_t>& a_targets, uint32_t a_index, uint8_t a_value)
		{
			if (a_targets[a_index] == a_value) {
				return false;
			}
			a_targets[a_index] = a_value;
			SetBit(g_fading, a_index);
			g_transition = true;
			return true;
		}

		// Texels from a_from, in ring order, before the next set bit, or a_limit when no bit is
		// set within the next a_limit texels.
		uint32_t Skip(const std::vector<uint64_t>& a_bits, uint32_t a_from, uint32_t a_limit)
		{
			uint32_t skipped = 0;
			while (skipped < a_limit) {
				const uint32_t index = (a_from + skipped) % (kTexels * kTexels);
				const uint64_t word = a_bits[index >> 6] >> (index & 63);
				if (word) {
					return std::min(skipped + static_cast<uint32_t>(std::countr_zero(word)), a_limit);
				}
				skipped += 64 - (index & 63);
			}
			return a_limit;
		}

		// Moving the window base only changes the cell of the columns and rows it wraps, so
		// only those texels go back into the queue.
		void QueueMoved(int32_t a_baseX, int32_t a_baseY)
		{
			const auto queueLines = [](int32_t a_from, int32_t a_to, bool a_rows) {
				const int32_t first = std::min(a_from, a_to);
				const int32_t lines = std::min(std::abs(a_to - a_from), static_cast<int32_t>(kTexels));
				for (int32_t k = 0; k < lines; ++k) {
					const uint32_t line = static_cast<uint32_t>(first + k) & kMask;
					for (uint32_t i = 0; i < kTexels; ++i) {
						const uint32_t index = a_rows ? line * kTexels + i : i * kTexels + line;
						SetBit(g_queued, index);
						ClearBit(g_holes, index);
					}
				}
			};
			queueLines(g_queuedBaseX, a_baseX, false);
			queueLines(g_queuedBaseY, a_baseY, true);
			g_queuedBaseX = a_baseX;
			g_queuedBaseY = a_baseY;
		}

		bool IsActorHit(const RE::hkpCollidable* a_collidable)
		{
			if (!a_collidable) {
				return false;
			}

			switch (a_collidable->GetCollisionLayer()) {
			case RE::COL_LAYER::kCharController:
			case RE::COL_LAYER::kBiped:
			case RE::COL_LAYER::kBipedNoCC:
			case RE::COL_LAYER::kDeadBip:
				return true;
			default:
				return false;
			}
		}

		float RayHeight(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ,
			float a_fromAbove, float a_toAbove)
		{
			const float scale = RE::bhkWorld::GetWorldScale();

			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_worldX * scale, a_worldY * scale,
				(a_landZ + a_fromAbove) * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_worldX * scale, a_worldY * scale,
				(a_landZ + a_toAbove) * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

			a_tes->Pick(pick);
			if (!pick.rayOutput.HasHit() || IsActorHit(pick.rayOutput.rootCollidable)) {
				return -1.0f;
			}

			return a_fromAbove +
				(a_toAbove - a_fromAbove) * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
		}

		bool Occluded(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ)
		{
			const float low = std::max(Settings::shelterClearance, 1.0f);
			return RayHeight(a_tes, a_worldX, a_worldY, a_landZ, low,
					   low + std::max(Settings::shelterHeight, 1.0f)) >= 0.0f;
		}

		uint8_t CapFor(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ)
		{
			const float raise = std::max(Settings::snowRaiseHeight, 1.0f);

			const float low = std::max(Settings::shelterFloorTolerance, 1.0f);
			const float high = raise + low;

			const float hit = RayHeight(a_tes, a_worldX, a_worldY, a_landZ, high, low);
			if (hit < 0.0f) {
				return 255;
			}

			return static_cast<uint8_t>(
				std::clamp(hit / raise, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		void Smooth(const std::vector<uint8_t>& a_src, std::vector<uint8_t>& a_dst)
		{
			const int n = static_cast<int>(kTexels);
			const int r = std::clamp(Settings::shelterSeamTexels, 0, n / 4);
			if (r <= 0) {
				a_dst = a_src;
				return;
			}

			BoxFilter::Apply<kTexels>(a_src.data(), a_dst.data(), g_rowSums.data(), r);
		}
	}

	bool Ready()
	{
		return g_texture && g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return false;
		}

		Allocate();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTexels;
		desc.Height = kTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = g_capSmooth.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			g_failed = true;
			logger::warn("Shelter: CreateTexture2D failed - the mesh cap is off");
			return false;
		}

		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			g_failed = true;
			logger::warn("Shelter: CreateShaderResourceView failed - the mesh cap is off");
			return false;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			g_failed = true;
			logger::warn("Shelter: CreateSamplerState failed - the mesh cap is off");
			return false;
		}

		logger::info("Shelter ready: {}x{} texels over {} world units ({} per texel); 0.35s temporal response",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	ID3D11ShaderResourceView* View()
	{
		return Ready() ? g_srv : nullptr;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !Ready()) {
			return;
		}
		a_context->DSSetShaderResources(kSlot, 1, &g_srv);
		a_context->DSSetSamplers(kSamplerSlot, 1, &g_sampler);
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context) {
			return;
		}
		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
	}

	void Shutdown()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_srv);
		drop(g_sampler);
		drop(g_texture);
		g_failed = false;

		g_roofDisplay.clear();
		g_capDisplay.clear();
		g_roofVisible.clear();
		g_capVisible.clear();
		g_fading.clear();
		g_transition = false;
		g_cap.clear();
		g_capSmooth.clear();
		g_raw.clear();
		g_smooth.clear();
		g_rowSums.clear();
		g_filledX.clear();
		g_filledY.clear();
		g_queued.clear();
		g_holes.clear();
		g_refresh.clear();
		g_allocated = false;
		g_haveCentre = false;
		g_dirty = false;
		++g_revision;
	}

	void Reset()
	{
		if (!g_allocated) {
			return;
		}

		std::fill(g_filledX.begin(), g_filledX.end(), -0x40000000);
		std::fill(g_filledY.begin(), g_filledY.end(), -0x40000000);
		std::fill(g_queued.begin(), g_queued.end(), ~0ull);
		std::fill(g_holes.begin(), g_holes.end(), 0ull);
		std::fill(g_refresh.begin(), g_refresh.end(), 0ull);
		g_cursor = 0;
		g_ageCursor = 0;
		g_holeCursor = 0;

		std::fill(g_cap.begin(), g_cap.end(), static_cast<uint8_t>(255));
		std::fill(g_raw.begin(), g_raw.end(), static_cast<uint8_t>(0));
		std::fill(g_fading.begin(), g_fading.end(), ~0ull);

		g_transition = true;
		g_capDirty = true;
	}

	uint32_t Revision()
	{
		return g_revision;
	}

	bool Settling()
	{
		return g_transition;
	}

	float CapAt(float a_worldX, float a_worldY)
	{

		if (!Settings::enableShelter || !Settings::shelterMeshCap || !g_allocated ||
			!g_haveCentre || g_capVisible.empty()) {
			return 1.0f;
		}

		const float u = (a_worldX / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const float v = (a_worldY / kWorldSize) * static_cast<float>(kTexels) - 0.5f;

		const float u0f = std::floor(u);
		const float v0f = std::floor(v);

		const auto warp = [](float a_f) { return a_f * a_f * (3.0f - 2.0f * a_f); };
		const float fx = warp(u - u0f);
		const float fy = warp(v - v0f);

		const uint32_t x0 = static_cast<uint32_t>(static_cast<int32_t>(u0f)) & kMask;
		const uint32_t y0 = static_cast<uint32_t>(static_cast<int32_t>(v0f)) & kMask;
		const uint32_t x1 = (x0 + 1) & kMask;
		const uint32_t y1 = (y0 + 1) & kMask;

		const auto texel = [](uint32_t a_x, uint32_t a_y) {
			return static_cast<float>(
					   g_capVisible[(static_cast<size_t>(a_y) * kTexels) + a_x]) /
			       255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		float       cap = top + (bottom - top) * fy;

		float centreX = 0.0f, centreY = 0.0f, halfExtent = 0.0f;
		if (Clipmap::GetWindow(centreX, centreY, halfExtent)) {
			const float reach = std::max(std::fabs(a_worldX - centreX),
				std::fabs(a_worldY - centreY));
			const float start = kWorldSize * 0.39f;
			const float end = kWorldSize * 0.47f;
			const float release =
				std::clamp((reach - start) / std::max(end - start, 1e-3f), 0.0f, 1.0f);
			cap = cap + (1.0f - cap) * release;
		}

		return std::clamp(cap, 0.0f, 1.0f);
	}

	void ApplyOpen(const uint8_t* a_smooth, uint8_t* a_upload, uint32_t a_texels,
		int32_t a_baseX, int32_t a_baseY)
	{
		if (!Settings::enableShelter || !g_allocated || !g_haveCentre) {
			return;
		}

		// Only cells less than half the grid from the centre have a roof value; further out
		// the grid wraps onto them. Clip that square to the caller's window.
		const int32_t reach = static_cast<int32_t>(kTexels / 2) - 1;
		const int32_t last = static_cast<int32_t>(a_texels) - 1;
		const int32_t fromX = std::max(g_centreCellX - reach, a_baseX);
		const int32_t toX = std::min(g_centreCellX + reach, a_baseX + last);
		const int32_t fromY = std::max(g_centreCellY - reach, a_baseY);
		const int32_t toY = std::min(g_centreCellY + reach, a_baseY + last);

		const uint32_t mask = a_texels - 1;
		for (int32_t cellY = fromY; cellY <= toY; ++cellY) {
			const uint8_t* roofs =
				&g_smooth[static_cast<size_t>(static_cast<uint32_t>(cellY) & kMask) * kTexels];
			const size_t row = static_cast<size_t>(static_cast<uint32_t>(cellY) & mask) * a_texels;

			for (int32_t cellX = fromX; cellX <= toX; ++cellX) {
				const uint8_t roof = roofs[static_cast<uint32_t>(cellX) & kMask];
				const size_t  index = row + (static_cast<uint32_t>(cellX) & mask);
				// With no roof or no snow the copied value is already the result.
				if (roof == 0 || a_smooth[index] == 0) {
					continue;
				}

				const float open = 1.0f - static_cast<float>(roof) * (1.0f / 255.0f);
				a_upload[index] = static_cast<uint8_t>(static_cast<float>(a_smooth[index]) * open);
			}
		}
	}

	void Update()
	{
		if (!Settings::enableShelter || !Settings::enableSnowRaise) {
			return;
		}

		auto* player = globals::game::player;
		auto* tes = RE::TES::GetSingleton();
		if (!player || !tes) {
			return;
		}

		Allocate();

		if (Settings::shelterMeshCap) {
			Initialize();
		}

		const int64_t started = Profiler::Ticks();

		const RE::NiPoint3 position = player->GetPosition();

		const int32_t wasX = g_centreCellX;
		const int32_t wasY = g_centreCellY;

		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		if (!g_haveCentre || wasX != g_centreCellX || wasY != g_centreCellY) {
			++g_revision;
		}
		g_haveCentre = true;

		const int32_t baseX = g_centreCellX - static_cast<int32_t>(kTexels / 2);
		const int32_t baseY = g_centreCellY - static_cast<int32_t>(kTexels / 2);

		const uint32_t total = static_cast<uint32_t>(g_raw.size());

		QueueMoved(baseX, baseY);

		const uint32_t aging = static_cast<uint32_t>(std::max(Settings::shelterRefresh, 0));
		for (uint32_t i = 0; i < aging; ++i) {
			SetBit(g_refresh, g_ageCursor);
			SetBit(g_queued, g_ageCursor);
			ClearBit(g_holes, g_ageCursor);
			g_ageCursor = (g_ageCursor + 1) % total;
		}

		const uint32_t budget = static_cast<uint32_t>(std::max(Settings::shelterBudget, 1));

		static uint32_t reportedRays = 0, skippedRays = 0, roofChanges = 0, capChanges = 0;
		uint32_t rays = 0;
		uint32_t skipped = 0;
		uint32_t scanned = 0;

		const uint32_t raysPerCell = Settings::shelterMeshCap && Ready() ? 2u : 1u;
		const uint32_t limit = std::max(budget, raysPerCell);

		// A read that can see snow reaches roof and cap texels this many cells from a snowy
		// coverage texel: the coverage seam, the shelter seam and the bilinear footprint both
		// maps are sampled with. Wider seams make the check too slow, so they refresh all.
		const int32_t snowReach =
			std::clamp(Settings::snowSeamTexels, 0, static_cast<int>(SnowCoverage::kTexels / 4)) +
			std::clamp(Settings::shelterSeamTexels, 0, static_cast<int>(kTexels / 4)) + 1;
		const bool skipBare = Settings::shelterRefreshSnowOnly && snowReach <= kMaxSnowReach;
		// While SnowCoverage::At reads 1 everywhere, object contact and trench depth read CapAt
		// on bare ground too, so there the cap keeps its refresh.
		const bool refreshBareCap = !SnowCoverage::Mapped();

		// Once the queue is empty, the budget it did not need re-queues holes for one more lap,
		// at most once a frame, so land that streams in late is still found without holding up
		// texels that have land.
		bool       retried = false;
		const auto requeueHoles = [&]() {
			if (retried) {
				return false;
			}
			retried = true;
			for (uint32_t spare = (limit - rays) / raysPerCell; spare > 0; --spare) {
				const uint32_t skip = Skip(g_holes, g_holeCursor, total);
				if (skip >= total) {
					break;
				}
				const uint32_t index = (g_holeCursor + skip) % total;
				ClearBit(g_holes, index);
				SetBit(g_queued, index);
				g_holeCursor = (index + 1) % total;
				scanned = 0;
			}
			return scanned < total;
		};

		while (rays + raysPerCell <= limit && (scanned < total || requeueHoles())) {
			if (const uint32_t skip = Skip(g_queued, g_cursor, total - scanned)) {
				g_cursor = (g_cursor + skip) % total;
				scanned += skip;
				if (scanned >= total) {
					continue;
				}
			}

			const uint32_t index = g_cursor;
			g_cursor = (g_cursor + 1) % total;
			++scanned;
			ClearBit(g_queued, index);

			const uint32_t tx = index & kMask;
			const uint32_t ty = index / kTexels;

			const int32_t cellX =
				baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			const int32_t cellY =
				baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);

			const bool filled = g_filledX[index] == cellX && g_filledY[index] == cellY;
			if (filled && !TestBit(g_refresh, index)) {
				continue;
			}

			const RE::NiPoint3 probe{
				(static_cast<float>(cellX) + 0.5f) * kTexelSize,
				(static_cast<float>(cellY) + 0.5f) * kTexelSize,
				position.z
			};

			float landZ = 0.0f;
			if (!tes->GetLandHeight(probe, landZ)) {
				// A failed probe still costs no budget, but the texel is not probed again every
				// frame: it stays unfilled and waits in g_holes to be retried.
				SetBit(g_holes, index);
				continue;
			}
			ClearBit(g_refresh, index);

			// A refresh of a cell with no snow near it skips its rays, since no read that can
			// see snow reaches its targets for this cell (the lift class map also reads its cap
			// for the coverage 128 cells away, see ShelterRefreshSnowOnly). It still costs the
			// budget, so every other texel is probed on the same frame as before.
			const bool bare = skipBare && filled && SnowCoverage::NoSnowNear(cellX, cellY, snowReach);

			if (bare) {
				++skipped;
			} else {
				const bool occluded = Occluded(tes, probe.x, probe.y, landZ);

				const uint8_t value = occluded ? 255 : 0;
				if (SetTarget(g_raw, index, value)) {
					++roofChanges;
				}
			}
			++rays;

			if (Settings::shelterMeshCap && Ready()) {
				if (bare && !refreshBareCap) {
					++skipped;
				} else {
					const uint8_t cap = CapFor(tes, probe.x, probe.y, landZ);

					if (SetTarget(g_cap, index, cap)) {
						++capChanges;
					}
				}
				++rays;
			}

			g_filledX[index] = cellX;
			g_filledY[index] = cellY;
		}

		if (g_transition) {
			const float dt = globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
			const float alpha = ShelterTransition::Alpha(dt);
			bool pending = false;
			// A settled texel already shows its targets and would not change, so only the
			// fading ones are visited, and they leave the set once both have settled.
			for (size_t word = 0; word < g_fading.size(); ++word) {
				for (uint64_t bits = g_fading[word]; bits; bits &= bits - 1) {
					const size_t i = word * 64 + static_cast<size_t>(std::countr_zero(bits));
					bool fading = ShelterTransition::Advance(g_roofDisplay[i], g_raw[i], alpha);
					fading |= ShelterTransition::Advance(g_capDisplay[i], g_cap[i], alpha);
					const auto roof = static_cast<uint8_t>(g_roofDisplay[i] + 0.5f);
					const auto cap = static_cast<uint8_t>(g_capDisplay[i] + 0.5f);
					g_dirty |= roof != g_roofVisible[i];
					g_capDirty |= cap != g_capVisible[i];
					g_roofVisible[i] = roof;
					g_capVisible[i] = cap;
					if (fading) {
						pending = true;
					} else {
						ClearBit(g_fading, static_cast<uint32_t>(i));
					}
				}
			}
			g_transition = pending;
		}

		if (g_dirty) {
			Smooth(g_roofVisible, g_smooth);
			g_dirty = false;
			++g_revision;
		}

		if (g_capDirty && Ready()) {
			Smooth(g_capVisible, g_capSmooth);

			auto* context = globals::d3d::context;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (context && SUCCEEDED(context->Map(
								g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_capSmooth.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_capDirty = false;
			}
		}

		reportedRays += rays - skipped;
		skippedRays += skipped;
		static auto reportAt = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (now - reportAt >= std::chrono::seconds(5)) {
			if (Settings::logSnowCoverage) {
				logger::info("Shelter transitions: {} rays, {} skipped with no snow near, {} roof target changes, {} cap target changes; blending={}",
					reportedRays, skippedRays, roofChanges, capChanges, g_transition);
			}
			reportedRays = skippedRays = roofChanges = capChanges = 0;
			reportAt = now;
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kShelter, Profiler::Ticks() - started);
	}
}
