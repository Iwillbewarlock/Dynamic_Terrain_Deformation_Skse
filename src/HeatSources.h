// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

#include "Clipmap.h"

namespace RE
{
	class Actor;
	class TESObjectREFR;
}

namespace HeatSources
{

	bool StampFor(RE::TESObjectREFR* a_ref, Clipmap::Stamp& a_out);

	bool ForActor(RE::Actor* a_actor, Clipmap::Stamp& a_out);

	void Reset();
}
