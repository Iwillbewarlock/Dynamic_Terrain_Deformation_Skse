// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

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
