// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace Shelter
{

	inline constexpr uint32_t kTexels = 128;
	inline constexpr float    kWorldSize = 8192.0f;
	inline constexpr float    kTexelSize = kWorldSize / static_cast<float>(kTexels);

	static_assert(kTexelSize == 64.0f,
		"A shelter texel must be a coverage texel, or Combine has to resample");

	inline constexpr uint32_t kSlot = 3;
	inline constexpr uint32_t kSamplerSlot = 2;

	bool Initialize();
	bool Ready();

	void BindDomain(ID3D11DeviceContext* a_context);
	void UnbindDomain(ID3D11DeviceContext* a_context);

	ID3D11ShaderResourceView* View();

	void Shutdown();

	void Update();

	void Reset();

	uint32_t Revision();

	float AtCell(int32_t a_cellX, int32_t a_cellY);

	float CapAt(float a_worldX, float a_worldY);
}
