// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once
#include <algorithm>
#include <cstdint>

namespace ShelterScan
{
	enum class Result { kCached, kNoLand, kUpdated };
	struct Counts
	{
		uint32_t scanned{}, attempts{}, misses{}, rays{};
	};

	template <class Probe>
	Counts Run(uint32_t& cursor, uint32_t total, uint32_t budget, uint32_t raysPerCell, Probe&& probe)
	{
		budget = std::max(budget, 1u);
		raysPerCell = std::max(raysPerCell, 1u);
		const uint32_t rayLimit = std::max(budget, raysPerCell);
		Counts counts;
		while (counts.scanned < total && counts.attempts < budget &&
			counts.rays + raysPerCell <= rayLimit) {
			const uint32_t index = cursor;
			cursor = (cursor + 1) % total;
			++counts.scanned;
			const auto result = probe(index);
			if (result == Result::kCached) { continue; }
			++counts.attempts;
			if (result == Result::kNoLand) {
				++counts.misses;
			} else {
				counts.rays += raysPerCell;
			}
		}
		return counts;
	}
}
