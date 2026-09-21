// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ShelterTransition
{
	inline float Alpha(float dt)
	{
		if (!std::isfinite(dt) || dt <= 0.0f) { return 0.0f; }
		return -std::expm1(-std::min(dt, 0.1f) / 0.35f);
	}

	inline bool Advance(float& value, uint8_t target, float alpha)
	{
		value += (static_cast<float>(target) - value) * alpha;
		if (std::abs(value - target) < 0.01f) {
			value = target;
			return false;
		}
		return true;
	}
}
