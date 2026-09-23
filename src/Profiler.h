// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace Profiler
{

	enum class Scope
	{

		kClipmap,

		kLandscape,

		kLandscapeDepth,

		kOtherRouted,

		kCount
	};

	enum class Count
	{
		kLandscapeSeen,
		kLandscapeCulled,
		kLandscapeRouted,

		kDepthSeen,
		kDepthCulled,
		kDepthShadowSkipped,
		kDepthRouted,

		kActorRouted,

		kStaticSeen,
		kStaticRouted,
		kShaderBuilds,

		// Every SetupGeometry call the hooks see, and how many of them went on to the full
		// routing path instead of returning at the land and blood test.
		kHookedLighting,
		kHookedLightingFull,
		kHookedDepth,
		kHookedOtherUtility,
		kHookedUtilityFull,
		kCount
	};

	bool Enabled();

	void Frame(float a_rawDeltaSeconds);

	void GpuBegin(Scope a_scope);
	void GpuEnd();

	void Tally(Count a_what, uint32_t a_howMany = 1);

	enum class CpuScope
	{

		kDrawBracket,

		kGatherStamps,

		kSnowCoverage,

		kShelter,
		kObjectScan,
		kShaderPrepare,
		kMagicImpacts,

		kCount
	};

	int64_t Ticks();
	void    AddCpuTicks(CpuScope a_scope, int64_t a_ticks);

	void Reset();

	void Release();
}
