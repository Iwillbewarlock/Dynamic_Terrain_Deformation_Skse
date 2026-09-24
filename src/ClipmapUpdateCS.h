// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "Clipmap.h"

#include "Shelter.h"
#include "SnowCoverage.h"

#include <algorithm>
#include <format>
#include <string>

namespace Clipmap
{

	inline constexpr float kPrintCoreLo = 0.06f;
	inline constexpr float kPrintCoreHi = 0.96f;
	inline constexpr float kPrintBandRiseLo = 0.14f;
	inline constexpr float kPrintBandRiseHi = 0.34f;
	inline constexpr float kPrintBandFallLo = 0.52f;
	inline constexpr float kPrintBandFallHi = 0.84f;

	inline float PrintHeightFor(float a_mask, float a_depth, float a_rim)
	{
		const auto smoothstep = [](float a_lo, float a_hi, float a_x) {
			const float t = std::clamp((a_x - a_lo) / (a_hi - a_lo), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};

		const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, a_mask);
		const float band = std::clamp(smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, a_mask) -
										  smoothstep(kPrintBandFallLo, kPrintBandFallHi, a_mask),
			0.0f, 1.0f);

		return a_rim * band - a_depth * core;
	}

	// Fills StampBounds from the stamp rows and rim settings already in a_params: per
	// stamp, the world rectangle (min x, min y, max x, max y) that the texel reach test
	// in the update shader lets through, rim and swept span included. The shader
	// rejects thread groups against it. A group corner is a float, so it compares past
	// the rounded bound only when it is past the exact one, and the corners sit a cell
	// outside the group's texels: far more than a texel's own rounding of its distance,
	// or the rounding of reach here against the shader's. Where a NaN makes std::max or
	// std::clamp disagree with the shader's max and saturate, the bound comes out NaN,
	// which rejects nothing.
	//
	// A melt has no rim. When it is round (half width 0) and 0 <= shoulder < 1, its
	// weight 1 - smoothstep(r * shoulder, r, d) is exactly 0 from d >= r on, so its
	// rectangle stops at the radius and swept span. Any other melt keeps the rim reach.
	//
	// Also fills NoNoiseMask with the melt and print stamps: only the press branch reads
	// the rim and churn noise, so a group that only those reach skips it.
	template <class CB>
	void FillStampBounds(CB& a_params, uint32_t a_count)
	{
		static_assert(kMaxStamps <= 128, "NoNoiseMask holds one bit per stamp in a uint4");

		for (auto& word : a_params.noNoiseMask) {
			word = 0;
		}

		for (uint32_t i = 0; i < a_count; ++i) {
			const float* s = a_params.stamps[i];
			const float* p = a_params.stampParams[i];
			const float* motion = a_params.stampMotion[i];

			// The shader's branch tests: melt, print, and press for the rest (NaN too).
			const bool melt = p[2] > 0.5f && p[2] < 1.5f;
			if (melt || p[2] > 1.5f) {
				a_params.noNoiseMask[i >> 5] |= 1u << (i & 31);
			}
			const bool rimless =
				melt && p[0] >= 0.0f && p[0] < 1.0f && a_params.stampShape[i][2] == 0.0f;

			const bool  snow = motion[2] > 0.5f;
			const float span = snow ? a_params.snowRim[0] : a_params.control[3];
			const float lean = snow ? a_params.snowRim[2] : a_params.rimShape[0];

			const float reach = std::max(s[2], a_params.stampShape[i][2]) *
				(rimless ? 1.0f :
						   1.0f + std::max(span, 0.0f) * (1.0f + std::clamp(lean, 0.0f, 1.0f)));

			a_params.stampBounds[i][0] = s[0] + (std::min(0.0f, -motion[0]) - reach);
			a_params.stampBounds[i][1] = s[1] + (std::min(0.0f, -motion[1]) - reach);
			a_params.stampBounds[i][2] = s[0] + (std::max(0.0f, -motion[0]) + reach);
			a_params.stampBounds[i][3] = s[1] + (std::max(0.0f, -motion[1]) + reach);
		}
	}

	constexpr char kUpdateShader[] = R"(
RWTexture2D<float> Field : register(u0);

RWTexture2D<float2> DecayRate : register(u1);

RWTexture2D<uint> Activity : register(u2);

// One flag per 8x8 group: 1 when a texel of the group is not +0.0 after the update.
RWTexture2D<uint> GroupLive : register(u3);

Texture2D<float4> ShapeMask : register(t0);
SamplerState      ShapeSampler : register(s0);

Texture2D<float> CoarseField : register(t1);
Texture2D<float2> CoarseDecay : register(t2);

Texture2D<float> SnowCoverageMap : register(t3);
Texture2D<float> SnowMeshCapMap : register(t4);

#ifdef GROUP_LIST
// The groups to update are GroupList[0, GroupCount), built by kGroupListShader, and the
// dispatch is 64 groups wide. The slots past the count, at the end of the last row,
// write nothing.
StructuredBuffer<uint> GroupList : register(t5);
ByteAddressBuffer      GroupCount : register(t6);
#endif

// The same rows as StampBounds in Params, copied by Clipmap::Update. The group test below
// reads a different stamp in each thread, and constant buffer reads at different
// addresses in one wave take turns on NVIDIA; a buffer load does not.
Buffer<float4> StampBoundsBuffer : register(t7);

cbuffer Params : register(b0)
{

	float4 Window;

	float4 Control;

	float4 Weather;

	float4 Stamps[MAX_STAMPS];

	float4 StampParams[MAX_STAMPS];

	float4 StampShape[MAX_STAMPS];

	float4 StampMotion[MAX_STAMPS];

	float4 Coarse;

	float4 RimShape;

	float4 SnowRim;

	float4 Raise;

	float4 RaiseWindow;

	// The update reads its copy, StampBoundsBuffer. The group list pass reads these rows,
	// the same stamp in every thread, which a constant buffer serves faster than a buffer.
	float4 StampBounds[MAX_STAMPS];

	uint4 NoNoiseMask;

	// x: update groups per activity cell on each axis, y: activity texels per axis - 1.
	uint4 ActivityShape;
};

)"

		R"(

uint HashCell(int2 cell)
{
	uint2 q = uint2(cell) * uint2(1597334673u, 3812015801u);
	uint  n = (q.x ^ q.y) * 1597334673u;
	n ^= n >> 15;
	return n * 2246822519u;
}

float ValueNoise(float2 p)
{
	const float2 base = floor(p);
	float2       f = p - base;
	f = f * f * (3.0f - 2.0f * f);

	const int2 c = int2(base);
	const float a = (float)HashCell(c)                  * (1.0f / 4294967296.0f);
	const float b = (float)HashCell(c + int2(1, 0))     * (1.0f / 4294967296.0f);
	const float d = (float)HashCell(c + int2(0, 1))     * (1.0f / 4294967296.0f);
	const float e = (float)HashCell(c + int2(1, 1))     * (1.0f / 4294967296.0f);

	return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

float FractalNoise(float2 worldXY, float wavelength)
{
	const float2x2 turn = float2x2(0.7986f, -0.6018f, 0.6018f, 0.7986f);

	float2 p = worldXY / max(wavelength, 1e-3f);
	float  n = ValueNoise(p);

	p = mul(turn, p) * 2.03f;
	n += 0.5f * ValueNoise(p);

	return (n * (1.0f / 1.5f)) * 2.0f - 1.0f;
}

float RimNoise(float2 worldXY)
{
	return FractalNoise(worldXY, 6.0f);
}

float ChurnNoise(float2 worldXY)
{

	float n = FractalNoise(worldXY, 12.0f);

	n *= abs(n);

	const float patch = 0.65f + 0.35f * FractalNoise(worldXY, 70.0f);

	return n * patch;
}

float SnowBlanket(float2 worldXY)
{
	const float2 fromCentre = abs(worldXY - RaiseWindow.xy);
	const float  reach      = max(fromCentre.x, fromCentre.y);
	const float  fade       = 1.0f - saturate(
		(reach - RaiseWindow.z) / max(RaiseWindow.w - RaiseWindow.z, 1e-3f));
	if (fade <= 0.0f) {
		return 0.0f;
	}

	float2 uv = worldXY / kCoverageWorldSize;
	const float2 t = uv * kCoverageTexels + 0.5f;
	const float2 i = floor(t);
	float2 f = t - i;
	f = f * f * (3.0f - 2.0f * f);
	uv = (i + f - 0.5f) / kCoverageTexels;

	const float cover = SnowCoverageMap.SampleLevel(ShapeSampler, uv, 0.0f);
	const float snowy = smoothstep(0.5f, 1.0f, cover);

	float lift = snowy * Raise.x * Raise.y;

	if (Raise.w > 0.5f) {
		float2 cuv = worldXY / kMeshCapWorldSize;
		const float2 ct = cuv * kMeshCapTexels + 0.5f;
		const float2 ci = floor(ct);
		float2 cf = ct - ci;
		cf = cf * cf * (3.0f - 2.0f * cf);
		cuv = (ci + cf - 0.5f) / kMeshCapTexels;

		float cap = SnowMeshCapMap.SampleLevel(ShapeSampler, cuv, 0.0f);
		cap = lerp(cap, 1.0f, saturate(
			(reach - kMeshCapFadeStart) /
			max(kMeshCapFadeEnd - kMeshCapFadeStart, 1e-3f)));

		lift = min(lift, cap * Raise.x);
	}

	return lift * fade;
}

float2 SweptDelta(float2 worldXY, float4 s, float2 motion)
{
	const float2 delta = worldXY - s.xy;
	const float  lenSq = dot(motion, motion);
	if (lenSq <= 1e-4f) {
		return delta;
	}

	const float t = saturate(dot(delta + motion, motion) / lenSq);
	return delta + motion * (1.0f - t);
}

float PrintProfile(float2 worldXY, float4 s, float4 p, float4 shape, float2 motion)
{
	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float2 delta = SweptDelta(worldXY, s, motion);

	const float2 local = float2(dot(delta, right), dot(delta, forward));
	const float  halfWidth = max(shape.z, 1e-3f);
	const float  halfLength = max(s.z, 1e-3f);

	const float2 uv = float2(
		0.5f + (local.x * shape.w) / (halfWidth * 2.0f),
		0.5f - local.y / (halfLength * 2.0f));

	if (any(uv < 0.0f) || any(uv > 1.0f)) {
		return 0.0f;
	}

	const float m = ShapeMask.SampleLevel(ShapeSampler, uv, 0).r;

	const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, m);
	const float band = saturate(
		smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, m) -
		smoothstep(kPrintBandFallLo, kPrintBandFallHi, m));

	return p.w * band - s.w * core;
}

float StampDistance(float2 worldXY, float4 s, float4 shape, float2 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion);
	if (shape.z <= 0.0f) {
		return length(delta);
	}

	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float halfLength = max(s.z, 1e-3f);
	const float halfWidth = max(shape.z, 1e-3f);

	const float2 n = float2(dot(delta, forward) / halfLength,
		dot(delta, right) / halfWidth);

	return length(n) * halfLength;
}

)"

		R"(
groupshared uint gBlockMax;
groupshared uint gNegativeZero;

static const uint kStampWords = (MAX_STAMPS + 31) / 32;
groupshared uint gStampHits[kStampWords];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID,
	uint3 thread : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
	bool listed = true;
#ifdef GROUP_LIST
	const uint slot = gid.y * 64 + gid.x;
	listed = slot < GroupCount.Load(0);
	const uint packed = listed ? GroupList[slot] : 0;
	gid = uint3(packed & 0xFFFF, packed >> 16, 0);
	id = uint3(gid.xy * 8 + thread.xy, 0);
#endif

	if (groupIndex == 0) {
		gBlockMax = 0;
		gNegativeZero = 0;
	}
	if (groupIndex < kStampWords) {
		gStampHits[groupIndex] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	const int n = (int)Window.w;

	const int mask = n - 1;
	const int2 centreCell = int2(Window.xy);
	const int2 base = centreCell - (n >> 1);
	const int2 cell = base + int2(
		((int)id.x - base.x) & mask,
		((int)id.y - base.y) & mask);

	const float2 worldXY = float2(cell) * Window.z;

	float rimNoise = 0.0f;
	float churn = 0.0f;

	float h = Field[id.xy];
	float2 metadata = DecayRate[id.xy];
	float rate = metadata.x;
	float snow = metadata.y >= 0.5f ? 1.0f : 0.0f;

	if (Control.x > 0.0f) {
		h *= pow(max(saturate(rate), 1e-6f), Control.x);
	}

	if (Weather.x > 0.0f && Control.x > 0.0f) {
		h *= pow(max(1.0f - Weather.x, 0.0f), Control.x);
	}

)"
		R"(

	float melt = 0.0f;

	float meltFloor = 0.0f;

	float meltRate = 0.0f;

	float target = 0.0f;
	float targetRate = 0.0f;
	float targetSnow = 0.0f;

	const int count = min((int)Control.y, MAX_STAMPS);
	{
		// Each thread tests one stamp against every cell this group covers (the
		// whole window when the group straddles the wrap), padded by a cell so float
		// rounding can never drop a stamp that a texel below would accept. The CPU
		// turns the texel test below into one rectangle per stamp (FillStampBounds),
		// so a thread reads one row here instead of three, from StampBoundsBuffer.
		const int2 first = (int2(gid.xy * 8) - base) & mask;
		const bool2 wraps = first + 7 > mask;
		const float2 cornerA = float2(base + (wraps ? 0 : first) - 1) * Window.z;
		const float2 cornerB = float2(base + (wraps ? mask : first + 7) + 1) * Window.z;
		const float2 groupLo = min(cornerA, cornerB);
		const float2 groupHi = max(cornerA, cornerB);

		for (int i = (int)groupIndex; i < count; i += 64) {
			const float4 bounds = StampBoundsBuffer[i];
			if (any(groupHi < bounds.xy) || any(groupLo > bounds.zw)) {
				continue;
			}
			InterlockedOr(gStampHits[i >> 5], 1u << (i & 31));
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// Only a group with a press stamp in reach needs the noise (the melt and print
	// branches never read it). Inside the loop fxc hoisted it in front of every
	// texel, so it is evaluated here behind a branch.
	uint anyHit = 0;
	[unroll] for (uint w = 0; w < kStampWords; ++w) {
		anyHit |= gStampHits[w] & ~NoNoiseMask[w];
	}
	[branch] if (anyHit != 0) {
		rimNoise = max(Weather.w, SnowRim.y) > 0.0f ? RimNoise(worldXY) : 0.0f;
		churn = max(RimShape.y, SnowRim.w) > 0.0f ? ChurnNoise(worldXY) : 0.0f;
	}

	// Visit the listed stamps in ascending order: ties in the comparisons below
	// keep the earlier stamp, exactly as the full loop did.
	uint word = 0;
	uint hits = gStampHits[0];
	[loop] for (;;) {
		[loop] while (hits == 0 && word + 1 < kStampWords) {
			hits = gStampHits[++word];
		}
		if (hits == 0) {
			break;
		}
		const int i = (int)(word * 32 + firstbitlow(hits));
		hits &= hits - 1;

		const float4 s = Stamps[i];
		const float4 p = StampParams[i];
		const float2 motion = StampMotion[i].xy;
		const float stampSnow = StampMotion[i].z;
		const float4 rim = stampSnow > 0.5f ? SnowRim :
			float4(Control.w, Weather.w, RimShape.x, RimShape.y);

		const float2 delta = worldXY - s.xy;

		const float reach = max(s.z, StampShape[i].z) *
			(1.0f + max(rim.x, 0.0f) * (1.0f + saturate(rim.z)));
		const float2 lo = min(0.0f.xx, -motion) - reach;
		const float2 hi = max(0.0f.xx, -motion) + reach;
		if (any(delta < lo) || any(delta > hi)) {
			continue;
		}

		const float  d = StampDistance(worldXY, s, StampShape[i], motion);

		const float f = 1.0f - smoothstep(s.z * p.x, s.z, d);

		// FillStampBounds stops a round melt with 0 <= shoulder < 1 at its radius,
		// where f reaches 0. A melt rim, or a weight that reaches further, needs a
		// wider rectangle there.
		if (p.z > 0.5f && p.z < 1.5f) {
			const float meltHere = s.w * f;
			if (meltHere > melt) {
				melt = meltHere;

				meltFloor = p.w * f;
				meltRate = p.y;
			}
			continue;
		}

		if (p.z > 1.5f) {
			const float printed =
				PrintProfile(worldXY, s, p, StampShape[i], motion);
			if (abs(printed) > abs(target)) {
				target = printed;
				targetRate = p.y;
				targetSnow = stampSnow;
			}
			continue;
		}

		float profile = -s.w * f * max(1.0f + rim.w * churn, 0.0f);

		if (p.w > 0.0f && rim.x > 0.0f) {
			const float span = max(s.z * rim.x, 1e-4f);

			const float lean  = saturate(rim.z);
			const float reach = span * (d >= s.z ? 1.0f + lean : 1.0f - lean);
			const float u = (d - s.z) / max(reach, 1e-4f);

			const float bump = saturate(1.0f - u * u);

			profile += p.w * bump * bump * max(1.0f + rim.y * rimNoise, 0.0f);
		}

		if (abs(profile) > abs(target)) {
			target = profile;
			targetRate = p.y;
			targetSnow = stampSnow;
		}
	}

	if (target < 0.0f && target < h) {
		h = target;
		rate = targetRate;
		snow = targetSnow;
	} else if (target > 0.0f && target > h && h >= 0.0f) {
		h = target;
		rate = targetRate;
		snow = targetSnow;
	}

	if (melt > 0.0f && Control.x > 0.0f) {
		const float remains = pow(max(1.0f - melt, 0.0f), Control.x);
		h = meltFloor + (h - meltFloor) * remains;

		if (meltFloor < 0.0f) {
			rate = max(rate, meltRate);
			snow = 1.0f;
		}
	}

)"

		R"(

	const float repose = snow > 0.5f ? RimShape.z : Weather.z;
	if (Weather.y > 0.0f && repose > 0.0f) {
		const float maxStep = Window.z * Weather.y;

		const int2 im = int2(mask, mask);
		const float n0 = Field[uint2((int2(id.xy) + int2(-1, 0)) & im)];
		const float n1 = Field[uint2((int2(id.xy) + int2(1, 0)) & im)];
		const float n2 = Field[uint2((int2(id.xy) + int2(0, -1)) & im)];
		const float n3 = Field[uint2((int2(id.xy) + int2(0, 1)) & im)];

		float pull = 0.0f;
		pull += sign(n0 - h) * max(abs(n0 - h) - maxStep, 0.0f);
		pull += sign(n1 - h) * max(abs(n1 - h) - maxStep, 0.0f);
		pull += sign(n2 - h) * max(abs(n2 - h) - maxStep, 0.0f);
		pull += sign(n3 - h) * max(abs(n3 - h) - maxStep, 0.0f);

		h += pull * 0.25f * saturate(repose);
	}

	const int2 delta = abs(cell - centreCell);
	if (max(delta.x, delta.y) >= (int)Coarse.w) {
		if (Coarse.x > 0.5f) {

			const float2 uv = worldXY / max(Coarse.y, 1e-3f);

			const float o = (0.5f / Window.w) * Coarse.z;

			float sum = CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, -o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, -o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, o), 0).r;
			h = sum * 0.25f;

			const float2 coarseMetadata = CoarseDecay.SampleLevel(ShapeSampler, uv, 0);
			rate = coarseMetadata.x;
			snow = coarseMetadata.y >= 0.5f ? 1.0f : 0.0f;
		} else {
			h = 0.0f;
			rate = 0.0f;
			snow = 0.0f;
		}
	}

	if (Raise.x > 0.0f && snow > 0.5f && h < 0.0f) {
		h = max(h, -(SnowBlanket(worldXY) + Raise.z));
	}

	if (listed) {
		Field[id.xy] = h;
		DecayRate[id.xy] = float2(rate, snow);
	}

	// A max with zero changes nothing, so bare ground skips the atomics. A -0.0 (a
	// print that decayed away) still makes the group live: repose rewrites it as +0.0.
	const uint magnitude = asuint(abs(h));
	if (magnitude != 0) {
		InterlockedMax(gBlockMax, magnitude);
	} else if (asuint(h) != 0) {
		InterlockedOr(gNegativeZero, 1u);
	}
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0 && listed) {
		GroupLive[gid.xy] = (gBlockMax | gNegativeZero) != 0 ? 1u : 0u;
	}

	if (groupIndex == 0 && listed && gBlockMax != 0) {

		const int2 centre = int2(gid.xy / ActivityShape.x);
		const int  wrap = (int)ActivityShape.y;
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {

				InterlockedMax(
					Activity[uint2((centre + int2(dx, dy)) & wrap)], gBlockMax);
			}
		}
	}
}
)";

	inline std::string UpdateShaderSource()
	{
		return std::format(
				"static const float kPrintCoreLo = {:.6f};\n"
				"static const float kPrintCoreHi = {:.6f};\n"
				"static const float kPrintBandRiseLo = {:.6f};\n"
				"static const float kPrintBandRiseHi = {:.6f};\n"
				"static const float kPrintBandFallLo = {:.6f};\n"
				"static const float kPrintBandFallHi = {:.6f};\n"

				"static const float kCoverageWorldSize = {:.4f}f;\n"
				"static const float kCoverageTexels    = {:.1f}f;\n"
				"static const float kMeshCapWorldSize  = {:.4f}f;\n"
				"static const float kMeshCapTexels     = {:.1f}f;\n"

				"static const float kMeshCapFadeStart  = {:.4f}f;\n"
				"static const float kMeshCapFadeEnd    = {:.4f}f;\n",
				kPrintCoreLo, kPrintCoreHi, kPrintBandRiseLo, kPrintBandRiseHi,
				kPrintBandFallLo, kPrintBandFallHi,
				SnowCoverage::kWorldSize, static_cast<float>(SnowCoverage::kTexels),
				Shelter::kWorldSize, static_cast<float>(Shelter::kTexels),
				Shelter::kWorldSize * 0.39f, Shelter::kWorldSize * 0.47f) +
			kUpdateShader;
	}

	// Lists the groups of one level that the update can change this frame, one thread
	// per group, for the update compiled with GROUP_LIST and run by DispatchIndirect
	// (ClipmapSkipIdleGroups). A texel that holds +0.0 keeps it unless a stamp reaches
	// it, the ring reseeds it, or repose pulls it towards one of its four neighbours:
	// decay and weather fill multiply, melting needs a melt stamp, and the snow floor
	// only lifts h < 0. So a group is listed when a stamp rectangle overlaps it (the
	// update's own group test), when a cell of it is on the ring, or when it or a group
	// across one of its edges was live after the last update. Any other group would
	// write back what it holds. Its flag is cleared here; the update writes the flags
	// of the listed groups. Params must match kUpdateShader.
	constexpr char kGroupListShader[] = R"(
cbuffer Params : register(b0)
{
	float4 Window;
	float4 Control;
	float4 Weather;
	float4 Stamps[MAX_STAMPS];
	float4 StampParams[MAX_STAMPS];
	float4 StampShape[MAX_STAMPS];
	float4 StampMotion[MAX_STAMPS];
	float4 Coarse;
	float4 RimShape;
	float4 SnowRim;
	float4 Raise;
	float4 RaiseWindow;
	float4 StampBounds[MAX_STAMPS];
	uint4  NoNoiseMask;
	uint4  ActivityShape;
};

Texture2D<uint> LiveBefore : register(t5);

RWTexture2D<uint>        LiveAfter : register(u0);
RWByteAddressBuffer      GroupArgs : register(u1);
RWByteAddressBuffer      GroupCount : register(u2);
RWStructuredBuffer<uint> GroupList : register(u3);

groupshared uint gListed;
groupshared uint gFirst;

[numthreads(8, 8, 1)]
void main(uint3 group : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0) {
		gListed = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	const int n = (int)Window.w;
	const int mask = n - 1;
	const int2 base = int2(Window.xy) - (n >> 1);
	const int2 first = (int2(group.xy * 8) - base) & mask;
	const bool2 wraps = first + 7 > mask;

	// The ring is every cell Coarse.w or more cells from the centre on either axis. A
	// group across the wrap holds the window edge.
	const int2 fromCentre = max(abs(first - (n >> 1)), abs(first + 7 - (n >> 1)));
	bool listed = any(wraps) || max(fromCentre.x, fromCentre.y) >= (int)Coarse.w;

	const int2 at = int2(group.xy);
	const int2 wrap = (n >> 3) - 1;
	listed = listed || LiveBefore[at] != 0 ||
		LiveBefore[(at + int2(-1, 0)) & wrap] != 0 || LiveBefore[(at + int2(1, 0)) & wrap] != 0 ||
		LiveBefore[(at + int2(0, -1)) & wrap] != 0 || LiveBefore[(at + int2(0, 1)) & wrap] != 0;

	[branch] if (!listed) {
		const float2 cornerA = float2(base + (wraps ? 0 : first) - 1) * Window.z;
		const float2 cornerB = float2(base + (wraps ? mask : first + 7) + 1) * Window.z;
		const float2 groupLo = min(cornerA, cornerB);
		const float2 groupHi = max(cornerA, cornerB);

		const int count = min((int)Control.y, MAX_STAMPS);
		for (int i = 0; i < count && !listed; ++i) {
			const float4 bounds = StampBounds[i];
			listed = !(any(groupHi < bounds.xy) || any(groupLo > bounds.zw));
		}
	}

	uint slot = 0;
	if (listed) {
		InterlockedAdd(gListed, 1u, slot);
	} else {
		LiveAfter[group.xy] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	// One global add per 64 groups. The update dispatch is 64 wide; the rows each
	// block of slots opens add up to ceil(count / 64) in whatever order they land.
	if (groupIndex == 0 && gListed != 0) {
		uint firstSlot;
		GroupCount.InterlockedAdd(0, gListed, firstSlot);
		const uint rows = (firstSlot + gListed + 63) / 64 - (firstSlot + 63) / 64;
		if (rows != 0) {
			uint rowsBefore;
			GroupArgs.InterlockedAdd(4, rows, rowsBefore);
		}
		gFirst = firstSlot;
	}
	GroupMemoryBarrierWithGroupSync();

	if (listed) {
		GroupList[gFirst + slot] = group.x | (group.y << 16);
	}
}
)";

	// The lift class map of the flat-patch rule (TessellationSkipFlat), built from the snow
	// coverage and mesh cap views the domain shader samples, after both uploaded this frame.
	// SnowRaise at x filters texels c and c + 1 per axis, c = floor(x / 64 - 0.5) (its
	// smoothstep remap keeps the pair); class texel b covers c = 2b .. 2b + 3, that is texels
	// 2b .. 2b + 4, wrapping as the samplers do.
	// - kLiftNone: every coverage texel is below 128/255, so the filtered cover is at most 0.5
	//   and smoothstep(0.5, 1, cover) is exactly 0.
	// - kLiftFull: every coverage texel and, with MESH_CAP, every cap texel is 255, so both
	//   filter to exactly 1: the full lift.
	// - kLiftBound is always set, so a map that is not bound (reads 0) proves nothing.
	constexpr char kLiftClassShader[] = R"(
Texture2D<float>  SnowCoverageMap : register(t0);
Texture2D<float>  SnowMeshCapMap  : register(t1);
RWTexture2D<uint> LiftClass       : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	bool none = true;
	bool full = true;
	[unroll] for (int y = 0; y < 5; ++y) {
		[unroll] for (int x = 0; x < 5; ++x) {
			const int2  t = int2(id.xy) * 2 + int2(x, y);
			const float cover = SnowCoverageMap.Load(int3(t & kCoverageMask, 0));
			none = none && cover < 0.5f;
			full = full && cover >= 1.0f;
#ifdef MESH_CAP
			full = full && SnowMeshCapMap.Load(int3(t & kMeshCapMask, 0)) >= 1.0f;
#endif
		}
	}
	LiftClass[id.xy] = kLiftBound | (none ? kLiftNone : 0u) | (full ? kLiftFull : 0u);
}
)";

	inline std::string LiftClassShaderSource(bool a_meshCap)
	{
		static_assert(SnowCoverage::kTexels == 2 * kLiftClassTexels &&
						  SnowCoverage::kTexels % Shelter::kTexels == 0 &&
						  SnowCoverage::kTexelSize == Shelter::kTexelSize,
			"a class texel must cover the same coverage and cap texels wherever it wraps");

		return std::format(
				   "{}"
				   "static const int  kCoverageMask = {};\n"
				   "static const int  kMeshCapMask  = {};\n"
				   "static const uint kLiftBound    = {}u;\n"
				   "static const uint kLiftNone     = {}u;\n"
				   "static const uint kLiftFull     = {}u;\n",
				   a_meshCap ? "#define MESH_CAP 1\n" : "", SnowCoverage::kTexels - 1,
				   Shelter::kTexels - 1, kLiftBound, kLiftNone, kLiftFull) +
			kLiftClassShader;
	}
}
