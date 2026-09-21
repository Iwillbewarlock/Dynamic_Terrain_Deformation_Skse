// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

namespace UtilityRouting
{

	constexpr bool IsCameraDepth(uint32_t technique)
	{
		constexpr uint32_t depth = 1u << 13;
		constexpr uint32_t unsupported = (1u << 9) | (1u << 12) |
			(1u << 14) |
			(0xFu << 21) |
			(1u << 25) | (1u << 28);
		return (technique & depth) != 0 && (technique & unsupported) == 0;
	}

	inline bool IsPerspectiveProjection(const float* a_viewProj)
	{

		if (!a_viewProj) {
			return true;
		}

		const auto magnitude = [](float a_value) {
			return a_value < 0.0f ? -a_value : a_value;
		};

		constexpr float kFlat = 1.0e-4f;
		return magnitude(a_viewProj[3]) > kFlat ||
		       magnitude(a_viewProj[7]) > kFlat ||
		       magnitude(a_viewProj[11]) > kFlat;
	}
}
