// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "ActorShapes.h"

#include <algorithm>
#include <cmath>

namespace ActorShapes
{
	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius)
	{
		if (!a_object) {
			return false;
		}

		auto* rigid = a_object->body.get() ? a_object->body.get()->AsBhkRigidBody() : nullptr;
		if (!rigid) {
			return false;
		}

		auto* hkpRigid = skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
		if (!hkpRigid) {
			return false;
		}

		if (skyrim_cast<RE::hkpListShape*>(hkpRigid)) {
			return false;
		}

		RE::hkVector4 massCentre;
		rigid->GetCenterOfMassWorld(massCentre);

		float components[4];
		_mm_storeu_ps(components, massCentre.quad);

		a_centre = RE::NiPoint3(components[0], components[1], components[2]) *
		           RE::bhkWorld::GetWorldScaleInverse();

		return ExtractRadius(hkpRigid->collidable.GetShape(), a_radius);
	}

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius)
	{
		if (!a_shape) {
			return false;
		}

		const auto project = [a_shape](float a_x, float a_y, float a_z) {
			return a_shape->GetMaximumProjection(RE::hkVector4{ a_x, a_y, a_z, 0.0f }) *
			       RE::bhkWorld::GetWorldScaleInverse();
		};

		const float hx = 0.5f * (project(1.0f, 0.0f, 0.0f) - project(-1.0f, 0.0f, 0.0f));
		const float hy = 0.5f * (project(0.0f, 1.0f, 0.0f) - project(0.0f, -1.0f, 0.0f));
		const float hz = 0.5f * (project(0.0f, 0.0f, 1.0f) - project(0.0f, 0.0f, -1.0f));

		switch (a_shape->type) {
		case RE::hkpShapeType::kSphere:

			a_radius = hx;
			return true;

		case RE::hkpShapeType::kCapsule:

			a_radius = std::max({ hx, hy, hz });
			return true;

		case RE::hkpShapeType::kBox:

			a_radius = std::sqrt(hx * hx + hy * hy + hz * hz);
			return true;

		case RE::hkpShapeType::kCylinder: {

			const float radial = std::max(hx, hy);
			a_radius = std::sqrt(radial * radial + hz * hz);
			return true;
		}

		default:

			a_radius = std::max({ hx, hy, hz });
			return true;
		}
	}
}
