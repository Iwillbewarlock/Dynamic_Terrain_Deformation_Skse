// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstddef>
#include <cstdint>

namespace BoxFilter
{
	namespace detail
	{
		// Radius 1 or 2: each sum is written out tap by tap, so the compiler vectorises the
		// loops, and the division is by a constant.
		template <uint32_t N, int R>
		void Fixed(const uint8_t* a_src, uint8_t* a_dst, uint32_t* a_rowSums)
		{
			static_assert(R == 1 || R == 2);
			static_assert(N > 2 * R);
			constexpr uint32_t mask = N - 1;
			constexpr uint32_t area = (2 * R + 1) * (2 * R + 1);

			for (uint32_t y = 0; y < N; ++y) {
				const uint8_t* src = a_src + static_cast<size_t>(y) * N;
				uint32_t*      dst = a_rowSums + static_cast<size_t>(y) * N;

				const auto wrapped = [&](uint32_t a_x) {
					uint32_t sum = 0;
					for (int k = -R; k <= R; ++k) {
						sum += src[(a_x + static_cast<uint32_t>(k)) & mask];
					}
					dst[a_x] = sum;
				};

				for (uint32_t x = 0; x < R; ++x) {
					wrapped(x);
				}
				for (uint32_t x = R; x < N - R; ++x) {
					uint32_t sum = static_cast<uint32_t>(src[x - 1]) + src[x] + src[x + 1];
					if constexpr (R == 2) {
						sum += static_cast<uint32_t>(src[x - 2]) + src[x + 2];
					}
					dst[x] = sum;
				}
				for (uint32_t x = N - R; x < N; ++x) {
					wrapped(x);
				}
			}

			for (uint32_t y = 0; y < N; ++y) {
				const auto row = [&](int a_offset) {
					return a_rowSums + static_cast<size_t>((y + static_cast<uint32_t>(a_offset)) & mask) * N;
				};
				const uint32_t* above = row(-1);
				const uint32_t* centre = row(0);
				const uint32_t* below = row(1);
				const uint32_t* above2 = row(-R);
				const uint32_t* below2 = row(R);

				uint8_t* dst = a_dst + static_cast<size_t>(y) * N;
				for (uint32_t x = 0; x < N; ++x) {
					uint32_t sum = above[x] + centre[x] + below[x];
					if constexpr (R == 2) {
						sum += above2[x] + below2[x];
					}
					dst[x] = static_cast<uint8_t>(sum / area);
				}
			}
		}

		// Any radius: sliding sums along each row, then down each column.
		template <uint32_t N>
		void Sliding(const uint8_t* a_src, uint8_t* a_dst, uint32_t* a_rowSums, int a_radius)
		{
			constexpr int      n = static_cast<int>(N);
			constexpr uint32_t kMask = N - 1;
			const int          r = a_radius;

			const int      width = 2 * r + 1;
			const uint32_t area = static_cast<uint32_t>(width) * static_cast<uint32_t>(width);

			for (int y = 0; y < n; ++y) {
				const uint8_t* src = &a_src[static_cast<size_t>(y) * n];
				uint32_t*      dst = &a_rowSums[static_cast<size_t>(y) * n];

				uint32_t sum = 0;
				for (int k = -r; k <= r; ++k) {
					sum += src[static_cast<uint32_t>(k) & kMask];
				}
				for (int x = 0; x < n; ++x) {
					dst[x] = sum;
					sum -= src[static_cast<uint32_t>(x - r) & kMask];
					sum += src[static_cast<uint32_t>(x + r + 1) & kMask];
				}
			}

			for (int x = 0; x < n; ++x) {
				uint32_t sum = 0;
				for (int k = -r; k <= r; ++k) {
					sum += a_rowSums[(static_cast<size_t>(static_cast<uint32_t>(k) & kMask) * n) +
						static_cast<size_t>(x)];
				}
				for (int y = 0; y < n; ++y) {
					a_dst[(static_cast<size_t>(y) * n) + static_cast<size_t>(x)] =
						static_cast<uint8_t>(sum / area);
					sum -= a_rowSums[(static_cast<size_t>(static_cast<uint32_t>(y - r) & kMask) * n) +
						static_cast<size_t>(x)];
					sum += a_rowSums[(static_cast<size_t>(static_cast<uint32_t>(y + r + 1) & kMask) * n) +
						static_cast<size_t>(x)];
				}
			}
		}
	}

	// Mean of the (2r+1) x (2r+1) texels around each texel of an N x N map that wraps at its
	// edges, truncated. a_rowSums is scratch for N x N sums; a_radius is 1 to N/4. Every path
	// adds the same bytes and divides by the same area, so radius 1 (the INI default) and 2
	// only take a faster route to the same bytes.
	template <uint32_t N>
	void Apply(const uint8_t* a_src, uint8_t* a_dst, uint32_t* a_rowSums, int a_radius)
	{
		static_assert((N & (N - 1)) == 0, "N must be a power of two for the toroidal index to be a bitmask");

		switch (a_radius) {
		case 1:
			detail::Fixed<N, 1>(a_src, a_dst, a_rowSums);
			break;
		case 2:
			detail::Fixed<N, 2>(a_src, a_dst, a_rowSums);
			break;
		default:
			detail::Sliding<N>(a_src, a_dst, a_rowSums, a_radius);
			break;
		}
	}
}
