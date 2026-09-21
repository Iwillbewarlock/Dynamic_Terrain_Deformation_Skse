// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace ActorShapes
{

	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius);

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius);
}
