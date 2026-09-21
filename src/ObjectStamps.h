// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

#include "Clipmap.h"

#include <vector>

namespace ObjectStamps
{

	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor,
		std::vector<Clipmap::Stamp>& a_out);

	void Reset();
}
