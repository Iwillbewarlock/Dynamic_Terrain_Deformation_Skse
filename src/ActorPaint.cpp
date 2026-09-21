// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#include "PCH.h"

#include "ActorPaint.h"
#include "Globals.h"
#include "Settings.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace ActorPaint
{
	namespace
	{
		struct State
		{
			Coat coat{};

			RE::NiPoint3 lastPosition{};
			bool         havePosition{ false };

			uint32_t idleFrames{ 0 };
		};

		std::mutex                              g_lock;
		std::unordered_map<RE::FormID, State>   g_states;

		constexpr uint32_t kEvictAfterFrames = 600;

		void CollectActors(std::vector<RE::ActorPtr>& a_out)
		{
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					auto actor = handle.get();
					if (actor && actor.get() && actor->Is3DLoaded()) {
						a_out.push_back(actor);
					}
				}
			}

			if (auto* player = globals::game::player) {
				if (auto handle = player->GetHandle().get()) {
					a_out.push_back(handle);
				}
			}
		}

		std::unordered_map<RE::FormID, bool> g_reported;
		constexpr float                      kReportAt = 0.15f;

		void LogCoat(RE::Actor* a_actor, Surfaces::Type a_surface, const Coat& a_coat)
		{
			if (!Settings::logActorPaint || a_coat.amount < kReportAt) {
				return;
			}
			if (g_reported.size() >= 32 || !g_reported.try_emplace(a_actor->GetFormID(), true).second) {
				return;
			}

			logger::info("Actor paint: {} reached {:.2f} on {} colour=({:.2f}, {:.2f}, {:.2f})",
				a_actor->GetName(), a_coat.amount, Surfaces::Name(a_surface),
				a_coat.colour[0], a_coat.colour[1], a_coat.colour[2]);
		}

		float g_traceTimer{ 0.0f };

		void TracePlayer(RE::Actor* a_actor, Surfaces::Type a_surface, const State& a_state,
			float a_speed, float a_dt)
		{
			if (!Settings::logActorPaint || !a_actor->IsPlayerRef()) {
				return;
			}

			g_traceTimer += a_dt;
			if (g_traceTimer < 1.0f) {
				return;
			}
			g_traceTimer = 0.0f;

			auto* root = a_actor->Get3D(false);

			logger::info(
				"Player coat: amount={:.3f} cling={:.2f} on {:<7} speed={:.0f} | "
				"anchor={:.1f} h={:.0f} | ref={:.1f} node={:.1f}",
				a_state.coat.amount, a_state.coat.cling, Surfaces::Name(a_surface), a_speed,
				a_state.coat.feetZ, a_state.coat.height, a_actor->GetPosition().z,
				root ? root->world.translate.z : 0.0f);
		}

		void Anchor(RE::Actor* a_actor, const RE::NiPoint3& a_position, Coat& a_coat)
		{
			a_coat.feetZ = a_position.z;
			a_coat.height = 0.0f;

			auto* root = a_actor->Get3D(false);
			if (!root) {
				return;
			}

			const auto& bound = root->worldBound;
			if (bound.radius > 1.0f) {
				a_coat.feetZ = bound.center.z - bound.radius;
				a_coat.height = bound.radius * 2.0f;
				return;
			}

			a_coat.feetZ = root->world.translate.z;
		}

		void Advance(RE::Actor* a_actor, State& a_state, float a_dt)
		{
			const RE::NiPoint3 position = a_actor->GetPosition();

			float speed = 0.0f;
			if (a_state.havePosition && a_dt > 0.0f) {
				speed = std::sqrt(a_state.lastPosition.GetSquaredDistance(position)) / a_dt;
			}
			a_state.lastPosition = position;
			a_state.havePosition = true;

			Anchor(a_actor, position, a_state.coat);

			const auto  surface = Surfaces::GroundAt(position).type;
			const auto& paint = Surfaces::PaintFor(surface);

			if (paint.rate > 0.0f) {
				const float moving = std::clamp(speed / Settings::paintFullSpeed, 0.0f, 1.0f);
				const float add = paint.rate * Settings::paintPickupRate * a_dt *
				                  std::lerp(Settings::paintStandingScale, 1.0f, moving);

				if (add > 0.0f) {
					const float carried = a_state.coat.amount;
					const float total = std::min(carried + add, 1.0f);

					const float weight = carried <= 1e-4f ?
						1.0f :
						std::clamp(add / std::max(total, 1e-4f) * Settings::paintBlendRate,
							0.0f, 1.0f);

					for (size_t i = 0; i < 3; ++i) {
						a_state.coat.colour[i] =
							std::lerp(a_state.coat.colour[i], paint.colour[i], weight);
					}

					a_state.coat.cling = std::lerp(a_state.coat.cling, paint.cling, weight);
					a_state.coat.gain = std::lerp(a_state.coat.gain, paint.gain, weight);

					a_state.coat.amount = total;
				}
			}

			const float keep = std::lerp(Settings::paintKeepPerSecond,
				Settings::paintKeepRunning,
				std::clamp(speed / Settings::paintFullSpeed, 0.0f, 1.0f));

			const float contact = std::clamp(paint.rate, 0.0f, 1.0f);
			const float effective = std::lerp(keep, 1.0f, contact);

			a_state.coat.amount *= std::pow(std::clamp(effective, 0.0f, 1.0f), a_dt);

			if (a_state.coat.amount < 0.002f) {
				a_state.coat.amount = 0.0f;
			}

			LogCoat(a_actor, surface, a_state.coat);
			TracePlayer(a_actor, surface, a_state, speed, a_dt);
		}
	}

	void Update(float a_deltaSeconds)
	{
		if (!Settings::enableActorPaint) {
			return;
		}

		const float dt = std::clamp(a_deltaSeconds, 0.0f, 0.25f);

		std::vector<RE::ActorPtr> actors;
		CollectActors(actors);

		const std::scoped_lock lock(g_lock);

		for (auto& [id, state] : g_states) {
			++state.idleFrames;
		}

		for (const auto& actor : actors) {
			if (!actor) {
				continue;
			}

			auto& state = g_states[actor->GetFormID()];
			state.idleFrames = 0;

			if (dt > 0.0f) {
				Advance(actor.get(), state, dt);
			} else if (state.coat.amount > 0.0f) {
				Anchor(actor.get(), actor->GetPosition(), state.coat);
			}
		}

		std::erase_if(g_states, [](const auto& a_entry) {
			return a_entry.second.idleFrames > kEvictAfterFrames;
		});
	}

	bool For(RE::FormID a_id, Coat& a_out)
	{
		if (!Settings::enableActorPaint) {
			return false;
		}

		const std::scoped_lock lock(g_lock);

		const auto it = g_states.find(a_id);
		if (it == g_states.end()) {
			return false;
		}

		a_out = it->second.coat;
		return it->second.coat.amount > 0.0f;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_states.clear();
		g_reported.clear();
	}
}
