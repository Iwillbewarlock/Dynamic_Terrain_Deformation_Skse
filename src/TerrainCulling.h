// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <utility>

namespace TerrainCulling
{
	class Bracket
	{
		using State = Microsoft::WRL::ComPtr<ID3D11RasterizerState>;
		struct Entry { State source, replacement; };
		std::vector<Entry> cache;
		State requested;
		bool active{};
	public:
		bool Active() const { return active; }
		void Begin(ID3D11DeviceContext* context)
		{
			context->RSGetState(requested.ReleaseAndGetAddressOf());
			active = true;
		}
		ID3D11RasterizerState* Select(ID3D11Device* device, ID3D11RasterizerState* state)
		{
			if (!active) { return state; }

			requested = state;
			if (!state) { return nullptr; }
			D3D11_RASTERIZER_DESC desc{}; state->GetDesc(&desc);
			if (desc.CullMode != D3D11_CULL_NONE) { return state; }
			for (const auto& entry : cache) {
				if (entry.source.Get() == state) { return entry.replacement.Get(); }
			}
			if (cache.size() >= 128) { return state; }
			desc.CullMode = D3D11_CULL_BACK;
			Entry entry; entry.source = state;
			if (FAILED(device->CreateRasterizerState(&desc, &entry.replacement))) { return state; }
			cache.push_back(std::move(entry));
			return cache.back().replacement.Get();
		}
		void End(ID3D11DeviceContext* context)
		{
			if (!active) { return; }
			active = false;
			context->RSSetState(requested.Get());
			requested.Reset();
		}
		void Reset() { cache.clear(); }
	};
}
