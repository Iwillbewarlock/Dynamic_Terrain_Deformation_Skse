// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once
#include "Clipmap.h"
#include <vector>

namespace RE { class NiPoint3; class MagicItem; }
namespace MagicImpacts
{
	void Install();
	void Reset();
	bool IsFireMagic(RE::MagicItem* spell);
	void Append(const RE::NiPoint3& anchor, std::vector<Clipmap::Stamp>& out);
}
