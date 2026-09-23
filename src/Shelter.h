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

	bool Settling();

	void Reset();

	uint32_t Revision();

	// Sets a_upload[i] = a_smooth[i] * (1 - roof) for every texel of a toroidal window of
	// a_texels (a power of two) cells from a_baseX/Y that has a roof. Texels without one are
	// left as they are, so the caller copies a_smooth into a_upload first.
	void ApplyOpen(const uint8_t* a_smooth, uint8_t* a_upload, uint32_t a_texels,
		int32_t a_baseX, int32_t a_baseY);

	float CapAt(float a_worldX, float a_worldY);
}
