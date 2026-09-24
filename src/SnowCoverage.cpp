// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include <chrono>

#include "BoxFilter.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SurfaceTypes.h"
#include "SurfaceProfiles.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace SnowCoverage
{
	namespace
	{
		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };

		std::vector<uint8_t> g_coverage;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_upload;

		uint32_t g_shelterRevision{ 0 };

		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		uint32_t g_cursor{ 0 };

		// One bit per texel, set for every texel that is not filled for the current window, so
		// the sweep can jump over filled texels instead of visiting all of them.
		std::vector<uint64_t> g_queued;
		int32_t               g_queuedBaseX{ 0 };
		int32_t               g_queuedBaseY{ 0 };

		int32_t g_centreCellX{ 0 };
		int32_t g_centreCellY{ 0 };

		int32_t g_settledCellX{ 0 };
		int32_t g_settledCellY{ 0 };
		bool    g_settledValid{ false };
		bool    g_dirty{ false };
		bool    g_everFilled{ false };

		bool g_reportedComplete{ false };

		bool Fail(const char* a_why)
		{
			logger::error("SnowCoverage: {}", a_why);
			g_failed = true;
			Shutdown();
			return false;
		}

		void SetBit(std::vector<uint64_t>& a_bits, uint32_t a_index)
		{
			a_bits[a_index >> 6] |= 1ull << (a_index & 63);
		}

		void ClearBit(std::vector<uint64_t>& a_bits, uint32_t a_index)
		{
			a_bits[a_index >> 6] &= ~(1ull << (a_index & 63));
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
						SetBit(g_queued, a_rows ? line * kTexels + i : i * kTexels + line);
					}
				}
			};
			queueLines(g_queuedBaseX, a_baseX, false);
			queueLines(g_queuedBaseY, a_baseY, true);
			g_queuedBaseX = a_baseX;
			g_queuedBaseY = a_baseY;
		}
	}

	bool Ready()
	{
		return g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return Fail("no device");
		}

		g_coverage.assign(static_cast<size_t>(kTexels) * kTexels, 0);
		g_settledValid = false;
		g_smooth.assign(g_coverage.size(), 0);
		g_upload.assign(g_coverage.size(), 0);
		g_rowSums.assign(g_coverage.size(), 0);
		g_shelterRevision = 0;

		g_filledX.assign(g_coverage.size(), -0x40000000);
		g_filledY.assign(g_coverage.size(), -0x40000000);
		g_queued.assign(g_coverage.size() / 64, ~0ull);

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
		initial.pSysMem = g_upload.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			return Fail("CreateTexture2D failed");
		}
		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			return Fail("CreateShaderResourceView failed");
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return Fail("CreateSamplerState failed");
		}

		logger::info("SnowCoverage ready: {}x{} texels over {} world units ({} per texel); profile-aware raise",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	void Shutdown()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_sampler);
		drop(g_srv);
		drop(g_texture);
	}

	void Reset()
	{
		std::fill(g_filledX.begin(), g_filledX.end(), -0x40000000);
		std::fill(g_filledY.begin(), g_filledY.end(), -0x40000000);
		std::fill(g_queued.begin(), g_queued.end(), ~0ull);
		g_cursor = 0;
		g_everFilled = false;
		g_reportedComplete = false;
		g_settledValid = false;
	}

	namespace
	{

		void Smooth()
		{
			const int n = static_cast<int>(kTexels);
			const int r = std::clamp(Settings::snowSeamTexels, 0, n / 4);
			if (r <= 0) {
				g_smooth = g_coverage;
				return;
			}

			BoxFilter::Apply<kTexels>(g_coverage.data(), g_smooth.data(), g_rowSums.data(), r);
		}
	}

	namespace
	{

		void Combine(int32_t a_baseX, int32_t a_baseY)
		{
			// Where no roof covers a texel it is uploaded as smoothed, so only the texels under
			// the shelter window need scaling.
			std::copy(g_smooth.begin(), g_smooth.end(), g_upload.begin());
			Shelter::ApplyOpen(g_smooth.data(), g_upload.data(), kTexels, a_baseX, a_baseY);
		}
	}

	void Update()
	{
		if (!Settings::enableSnowRaise) {
			return;
		}

		if (!Ready() && !Initialize()) {
			return;
		}

		auto* player = globals::game::player;
		auto* context = globals::d3d::context;
		if (!player || !context) {
			return;
		}

		const int64_t started = Profiler::Ticks();

		const RE::NiPoint3 position = player->GetPosition();

		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		const int32_t baseX = g_centreCellX - static_cast<int32_t>(kTexels / 2);
		const int32_t baseY = g_centreCellY - static_cast<int32_t>(kTexels / 2);

		QueueMoved(baseX, baseY);

		const uint32_t shelterRevision = Shelter::Revision();
		const bool     shelterMoved = shelterRevision != g_shelterRevision;
		const bool     settledHere = g_settledValid &&
			g_settledCellX == g_centreCellX && g_settledCellY == g_centreCellY;

		if (!g_dirty && !shelterMoved && settledHere) {
			Profiler::AddCpuTicks(Profiler::CpuScope::kSnowCoverage,
				Profiler::Ticks() - started);
			return;
		}

		const uint32_t budget = static_cast<uint32_t>(
			std::max(Settings::snowCoverageBudget, 1));

		uint32_t       queries = 0;
		uint32_t       scanned = 0;
		const uint32_t total = static_cast<uint32_t>(g_coverage.size());

		const bool sweep = !settledHere || g_dirty;

		while (sweep && queries < budget && scanned < total) {
			// Texels that are not queued are filled and would only be skipped below.
			if (const uint32_t skip = Skip(g_queued, g_cursor, total - scanned)) {
				g_cursor = (g_cursor + skip) % total;
				scanned += skip;
				if (scanned >= total) {
					break;
				}
			}

			const uint32_t index = g_cursor;
			g_cursor = (g_cursor + 1) % total;
			++scanned;

			const uint32_t tx = index & kMask;
			const uint32_t ty = index / kTexels;

			const int32_t cellX =
				baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			const int32_t cellY =
				baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);

			if (g_filledX[index] == cellX && g_filledY[index] == cellY) {
				ClearBit(g_queued, index);
				continue;
			}

			RE::NiPoint3 probe{
				(static_cast<float>(cellX) + 0.5f) * kTexelSize,
				(static_cast<float>(cellY) + 0.5f) * kTexelSize,
				position.z
			};

			++queries;
			float landZ = 0.0f;
			auto* tes = RE::TES::GetSingleton();
			if (!tes || !tes->GetLandHeight(probe, landZ)) {

				continue;
			}

			probe.z = landZ;
			const auto ground = Surfaces::GroundAt(probe);
			g_coverage[index] = ground.type == Surfaces::Type::kSnow ? 255 : 0;
			g_filledX[index] = cellX;
			g_filledY[index] = cellY;
			ClearBit(g_queued, index);
			g_dirty = true;
		}

		if (sweep && scanned >= total && queries == 0) {
			g_settledValid = true;
			g_settledCellX = g_centreCellX;
			g_settledCellY = g_centreCellY;
		}

		if (g_dirty || shelterMoved) {

			if (g_dirty) {
				Smooth();
			}

			Combine(baseX, baseY);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {

				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_upload.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_dirty = false;

				g_shelterRevision = shelterRevision;
			}
		}

		Profiler::AddCpuTicks(Profiler::CpuScope::kSnowCoverage,
			Profiler::Ticks() - started);

		if (!Settings::logSnowCoverage) {
			return;
		}

		using Clock = std::chrono::steady_clock;
		static Clock::time_point backlogSince{};
		static bool warned{};
		const bool complete = scanned >= total && queries < budget;
		if (complete) {
			if (!g_everFilled || warned) {
				logger::info("SnowCoverage: swept clean at {} units a texel", kTexelSize);
			}
			g_reportedComplete = true;
			g_everFilled = true;
			backlogSince = {};
			warned = false;
		} else {
			g_reportedComplete = false;
			const auto now = Clock::now();
			if (backlogSince == Clock::time_point{}) { backlogSince = now; }
			if (!warned && now - backlogSince > std::chrono::seconds(2)) {
				warned = true;
				logger::info("SnowCoverage: still catching up after 2 seconds (budget {})", budget);
			}
		}
	}

	bool Mapped()
	{
		return Settings::enableSnowRaise && g_everFilled && !g_upload.empty();
	}

	bool NoSnowNear(int32_t a_cellX, int32_t a_cellY, int32_t a_radius)
	{
		// A wider square would wrap onto its own texels.
		if (g_coverage.empty() || a_radius < 0 || a_radius >= static_cast<int32_t>(kTexels / 2)) {
			return false;
		}

		for (int32_t cellY = a_cellY - a_radius; cellY <= a_cellY + a_radius; ++cellY) {
			const size_t row = static_cast<size_t>(static_cast<uint32_t>(cellY) & kMask) * kTexels;
			for (int32_t cellX = a_cellX - a_radius; cellX <= a_cellX + a_radius; ++cellX) {
				const size_t index = row + (static_cast<uint32_t>(cellX) & kMask);
				if (g_filledX[index] != cellX || g_filledY[index] != cellY || g_coverage[index] != 0) {
					return false;
				}
			}
		}
		return true;
	}

	float At(float a_worldX, float a_worldY)
	{

		if (!Mapped()) {
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
					   g_upload[(static_cast<size_t>(a_y) * kTexels) + a_x]) /
			       255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		return top + (bottom - top) * fy;
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
		ID3D11SamplerState*       nullSampler = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
		a_context->DSSetSamplers(kSamplerSlot, 1, &nullSampler);
	}
}
