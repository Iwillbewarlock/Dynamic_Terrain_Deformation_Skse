// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace ShelterTransition
{

	class FadeCells
	{
	public:
		void Reset(uint32_t cells)
		{
			_count = cells;
			_words.assign((cells + 63u) / 64u, 0);
		}
		void Activate(uint32_t index) { _words[index / 64] |= uint64_t{1} << (index % 64); }
		void ActivateAll()
		{
			for (auto& word : _words) { word = ~uint64_t{0}; }
			if (_count % 64) { _words.back() = (uint64_t{1} << (_count % 64)) - 1; }
		}

		template <class AdvanceCell>
		uint32_t Advance(AdvanceCell&& advance)
		{
			uint32_t visited = 0;
			for (uint32_t w = 0; w < _words.size(); ++w) {
				auto bits = _words[w];
				while (bits) {
					const auto bit = std::countr_zero(bits);
					const auto mask = uint64_t{1} << bit;
					bits &= bits - 1;
					++visited;
					if (!advance(w * 64 + bit)) { _words[w] &= ~mask; }
				}
			}
			return visited;
		}

	private:
		uint32_t _count{};
		std::vector<uint64_t> _words;
	};
}
