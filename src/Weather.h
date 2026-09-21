// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

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
