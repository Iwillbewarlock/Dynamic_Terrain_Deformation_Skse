// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "SnowSurface.h"

#include "Clipmap.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "Weather.h"

#include <algorithm>
#include <cmath>

namespace SnowSurface
{
	float BlanketFor(float a_snowy, float a_worldX, float a_worldY)
	{

		const float depth = Weather::SnowDepth();
		if (depth <= 0.0f) {
			return 0.0f;
		}

		float centreX = 0.0f;
		float centreY = 0.0f;
		float halfExtent = 0.0f;
		if (!Clipmap::GetWindow(centreX, centreY, halfExtent)) {
			return 0.0f;
		}

		const float reach =
			std::max(std::fabs(a_worldX - centreX), std::fabs(a_worldY - centreY));

		const float fadeEnd = Settings::snowRaiseDistance;
		const float fadeStart = std::max(fadeEnd - Settings::snowRaiseFadeBand, 0.0f);
		const float fade =
			1.0f - std::clamp((reach - fadeStart) / std::max(fadeEnd - fadeStart, 1e-3f),
						0.0f, 1.0f);
		if (fade <= 0.0f) {
			return 0.0f;
		}

		float snowy = std::clamp(a_snowy, 0.0f, 1.0f);

		if (Settings::shelterMeshCap) {
			snowy = std::min(snowy, Shelter::CapAt(a_worldX, a_worldY));
		}

		return depth * snowy * fade;
	}

	float LiftAt(float a_worldX, float a_worldY)
	{
		const float cover = SnowCoverage::At(a_worldX, a_worldY);
		const float t = std::clamp((cover - 0.5f) / 0.5f, 0.0f, 1.0f);
		return BlanketFor(t * t * (3.0f - 2.0f * t), a_worldX, a_worldY);
	}
}
