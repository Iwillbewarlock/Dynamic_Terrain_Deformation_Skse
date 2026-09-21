// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

namespace BlanketTessellation
{
	inline constexpr const char* source = R"(
float BlanketDetail(float edgeLength, float farFactor, float nearFactor,
    float blanketSpacing, float maxFactor, float activity)
{
    if (blanketSpacing <= 0.0f) { return lerp(farFactor, nearFactor, saturate(activity)); }
    float fine = max(farFactor, nearFactor);
    float base = farFactor;
    if (blanketSpacing > 0.0f) {
        base = max(base, min(fine, clamp(edgeLength / blanketSpacing, 1.0f, maxFactor)));
    }
    return lerp(base, fine, saturate(activity));
}
)";
}
