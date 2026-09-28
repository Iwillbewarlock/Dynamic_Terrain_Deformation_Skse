// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
// Community Shaders globals layout and references are credited in NOTICE.
// Provenance notice added 2026-09-28.

#pragma once

namespace globals
{
	namespace d3d
	{
		inline ID3D11Device*        device{ nullptr };
		inline ID3D11DeviceContext* context{ nullptr };
	}

	namespace game
	{
		inline RE::UI*              ui{ nullptr };
		inline RE::PlayerCharacter* player{ nullptr };
		inline const float*         deltaTime{ nullptr };
	}

	bool Initialize();

	bool Ready();
}
