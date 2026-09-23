// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace ActorShapes
{

	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius);

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius);

	// RE::BSVisit::TraverseScenegraphCollision, visiting the same objects in the same order and
	// stopping the same way, but holding the callback by reference. The CommonLib version takes
	// a std::function by value and copies it for every child it visits, which is a heap
	// allocation per scene graph object when the callback does not fit std::function's small
	// buffer (the actor stamp callback does not).
	template <class F>
	RE::BSVisit::BSVisitControl WalkCollision(RE::NiAVObject* a_object, F&& a_func)
	{
		auto result = RE::BSVisit::BSVisitControl::kContinue;

		if (!a_object) {
			return result;
		}

		auto collision = static_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get());
		if (collision) {
			result = a_func(collision);
			if (result == RE::BSVisit::BSVisitControl::kStop) {
				return result;
			}
		}

		auto node = a_object->AsNode();
		if (node) {
			for (auto& child : node->GetChildren()) {
				result = WalkCollision(child.get(), a_func);
				if (result == RE::BSVisit::BSVisitControl::kStop) {
					break;
				}
			}
		}

		return result;
	}
}
