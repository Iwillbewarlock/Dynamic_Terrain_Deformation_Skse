// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

namespace RE { class BSGeometry; }
namespace BloodDecals
{
	void Update();
	void Reset();
	bool Contains(RE::BSGeometry* geometry);
	void NoteDraw(bool routed);
}
