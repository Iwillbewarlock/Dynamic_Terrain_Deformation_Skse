// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "Globals.h"

namespace globals
{
	bool Initialize()
	{

		if (!d3d::device || !d3d::context) {
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& runtime = renderer->GetRuntimeData();
				d3d::device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
				d3d::context = reinterpret_cast<ID3D11DeviceContext*>(runtime.context);
			}
		}

		if (!game::ui) {
			game::ui = RE::UI::GetSingleton();
		}

		if (!game::player) {
			game::player = RE::PlayerCharacter::GetSingleton();
		}

		if (!game::deltaTime) {

			game::deltaTime = reinterpret_cast<const float*>(
				REL::RelocationID(523660, 410199).address());
		}

		return Ready();
	}

	bool Ready()
	{
		return d3d::device != nullptr && d3d::context != nullptr;
	}
}
