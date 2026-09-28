// SPDX-License-Identifier: GPL-3.0-only
// Modifications Copyright (c) 2026 NearMidnightNow (NMN).
// Adapted from Community Shaders src/Utils/ActorUtils.h (GPLv3).
// Upstream contributor: Alan Tse. See NOTICE.
// Declarations adapted in the 2026-09-21 public snapshot.
// Provenance notice added 2026-09-28.

#pragma once

namespace ActorShapes
{

	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius);

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius);
}
