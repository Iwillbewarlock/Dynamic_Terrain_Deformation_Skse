// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace CoverageShelter
{

	template <uint32_t N, uint32_t S>
	uint32_t Combine(const uint8_t* source, uint8_t* output, int32_t baseX, int32_t baseY,
		const uint8_t* shelter, int32_t centreX, int32_t centreY)
	{
		static_assert(N > 0 && S > 0 && !(N & (N - 1)) && !(S & (S - 1)));
		std::memcpy(output, source, static_cast<size_t>(N) * N);
		if (!shelter) { return 0; }
		const int64_t half = S / 2;
		const auto x0 = std::max<int64_t>(baseX, int64_t{centreX} - half + 1);
		const auto y0 = std::max<int64_t>(baseY, int64_t{centreY} - half + 1);
		const auto x1 = std::min<int64_t>(int64_t{baseX} + N, int64_t{centreX} + half);
		const auto y1 = std::min<int64_t>(int64_t{baseY} + N, int64_t{centreY} + half);
		uint32_t scaled = 0;
		for (auto y = y0; y < y1; ++y) {
			const auto row = (static_cast<uint32_t>(y) & (N - 1)) * N;
			const auto roofRow = (static_cast<uint32_t>(y) & (S - 1)) * S;
			for (auto x = x0; x < x1; ++x) {
				const auto index = row + (static_cast<uint32_t>(x) & (N - 1));
				const float open = 1.0f - static_cast<float>(shelter[roofRow + (static_cast<uint32_t>(x) & (S - 1))]) * (1.0f / 255.0f);
				output[index] = static_cast<uint8_t>(static_cast<float>(source[index]) * open);
				++scaled;
			}
		}
		return scaled;
	}
}
