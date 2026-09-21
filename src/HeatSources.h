// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

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
