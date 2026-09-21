// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

namespace RE
{
	class Actor;
}

namespace ActorPaint
{

	struct Coat
	{
		float colour[3]{ 0.0f, 0.0f, 0.0f };

		float amount{ 0.0f };

		float cling{ 0.0f };

		float gain{ 1.0f };

		float feetZ{ 0.0f };
		float height{ 0.0f };
	};

	void Update(float a_deltaSeconds);

	bool For(RE::FormID a_id, Coat& a_out);

	void Reset();
}
