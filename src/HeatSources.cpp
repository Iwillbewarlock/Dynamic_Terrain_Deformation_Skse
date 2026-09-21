// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "HeatSources.h"
#include "MagicImpacts.h"
#include "ImpactPatterns.h"

#include "Settings.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>

namespace HeatSources
{
	namespace
	{
		std::mutex                     g_lock;
		std::unordered_set<RE::FormID> g_reported;

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool IsHot(RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref->GetBaseObject();
			if (!base) {
				return false;
			}

			switch (base->GetFormType()) {
			case RE::FormType::Light:
				return true;
			case RE::FormType::Hazard:
				if (const auto* hazard = base->As<RE::BGSHazard>(); hazard && hazard->data.spell) {
					return MagicImpacts::IsFireMagic(hazard->data.spell);
				}
				break;

			case RE::FormType::Explosion:

				return false;

			default:
				break;
			}

			const auto* model = base->As<RE::TESModel>();
			if (!model) {
				return false;
			}

			const char* path = model->GetModel();
			if (!path || !*path) {
				return false;
			}

			return Surfaces::MatchesKeywords(Lowered(path), Settings::heatKeywords);
		}

		float ReachFor(float a_boundRadius)
		{
			return ImpactPatterns::BoundedRadius(
				std::max(Settings::heatRadius, a_boundRadius * Settings::heatRadiusScale),
				1.0f, Settings::heatMaxRadius);
		}

		float MeltFloor(Surfaces::Type a_surface)
		{
			if (!Settings::heatMeltsSnow || a_surface != Surfaces::Type::kSnow) {
				return 0.0f;
			}

			return -Weather::SnowDepth() *
				std::clamp(Settings::heatMeltDepthScale, 0.0f, 4.0f);
		}

		void Fill(Clipmap::Stamp& a_out, const RE::NiPoint3& a_position, float a_reach)
		{
			a_out = {};
			a_out.kind = Clipmap::Stamp::Kind::kMelt;
			a_out.x = a_position.x;
			a_out.y = a_position.y;
			a_out.radius = a_reach;

			a_out.depth = std::clamp(Settings::heatMeltPerSecond, 0.0f, 1.0f);

			a_out.shoulder = std::clamp(Settings::heatShoulder, 0.0f, 0.95f);

			const auto ground = Surfaces::GroundAt(a_position);
			const auto surface = ground.type;

			a_out.rim = MeltFloor(surface);

			a_out.decay = 1.0f;
			if (a_out.rim < 0.0f) {
				const auto& response = ground.response;
				a_out.decay = std::clamp(Settings::stampDecayPerSecond *
					response.decayScale * Weather::DecayScale(),
					0.0f, 0.9999f);
			}
		}

		void Log(const char* a_what, RE::TESForm* a_form, const Clipmap::Stamp& a_stamp)
		{
			if (!Settings::logHeatSources) {
				return;
			}

			const std::scoped_lock lock(g_lock);
			if (g_reported.size() >= 24 || !g_reported.insert(a_form->GetFormID()).second) {
				return;
			}

			logger::info(
				"Heat source: {} [{}] at ({:.0f}, {:.0f}) reach={:.0f} melt={:.2f} floor={:.0f}",
				a_form->GetName(), a_what, a_stamp.x, a_stamp.y, a_stamp.radius,
				a_stamp.depth, a_stamp.rim);
		}
	}

	bool StampFor(RE::TESObjectREFR* a_ref, Clipmap::Stamp& a_out)
	{
		if (!Settings::enableHeatSources || !a_ref || !IsHot(a_ref)) {
			return false;
		}

		auto* root = a_ref->Get3D();
		if (!root) {
			return false;
		}

		const RE::NiPoint3 position = a_ref->GetPosition();
		const float reach = ReachFor(root->worldBound.radius);
		if (!(reach > 0.0f)) { return false; }
		Fill(a_out, position, reach);

		Log("world", a_ref, a_out);
		return true;
	}

	bool ForActor(RE::Actor* a_actor, Clipmap::Stamp& a_out)
	{
		if (!Settings::enableHeatSources || !Settings::heatFromTorches || !a_actor) {
			return false;
		}

		auto* equipped = a_actor->GetEquippedObject(true);
		if (!equipped || equipped->GetFormType() != RE::FormType::Light) {
			return false;
		}

		const RE::NiPoint3 position = a_actor->GetPosition();
		Fill(a_out, position, Settings::heatTorchRadius);

		a_out.depth = std::clamp(
			Settings::heatMeltPerSecond * Settings::heatTorchScale, 0.0f, 1.0f);

		Log("torch", a_actor, a_out);
		return true;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_reported.clear();
	}
}
