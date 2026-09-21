// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

namespace Weather
{

	void Update();

	void Reset();

	float Level();

	float Precipitation();

	float DepthScale();
	float DecayScale();

	float RaiseScale();
	float SnowDepth();

	float FillPerSecond();

	const char* StateLine();
}
