// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "BloodDecals.h"
#include "BloodDecalFilter.h"
#include "Settings.h"
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <array>

namespace BloodDecals
{
	namespace
	{

		std::unordered_map<RE::BSGeometry*, RE::NiPointer<RE::BSTempEffect>> targets;
		std::unordered_set<std::string> reported;
		std::unordered_set<std::string> reportedDrawTextures;
		bool reportedDraw{}, reportedSkip{}, reportedStart{};
		std::unordered_set<RE::BSTempEffect*> visited;
		std::array<size_t, 6> lastCounts{};
		std::chrono::steady_clock::time_point nextCensus{};
		// Everything the texture half of Classify() reads from a listed effect: which effect, its
		// class and its texture sets. Stored readings are compared, never followed.
		struct Entry
		{
			RE::BSTempEffect* effect{};
			const RE::NiRTTI* type{};
			RE::BGSTextureSet* first{};
			RE::BGSTextureSet* second{};
			bool operator==(const Entry&) const = default;
		};
		struct Scan
		{
			std::vector<Entry> entries;
			std::vector<std::uint32_t> blood;
		};
		std::unordered_map<RE::BGSDecalNode*, Scan> scanned;
		std::string prefixSource;
		BloodDecalFilter::Prefixes prefixes;
		const char* Diffuse(RE::BGSTextureSet* set)
		{
			return set ? set->textures[0].textureName.c_str() : "";
		}
		bool Blood(RE::BGSTextureSet* set)
		{
			const char* path = Diffuse(set);
			if (prefixSource != Settings::bloodDecalTexturePrefixes) {
				prefixSource = Settings::bloodDecalTexturePrefixes;
				prefixes = BloodDecalFilter::Parse(prefixSource);
			}
			return path && BloodDecalFilter::Matches(path, prefixes);
		}
		bool Terrain(RE::BSGeometry* geometry)
		{
			if (!geometry || geometry->GetGeometryRuntimeData().skinInstance) { return false; }
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			auto* material = property ? property->GetBaseMaterial() : nullptr;
			return material && material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
		}
		// `reported` only feeds the log, and logging can only be switched on by a settings
		// reload, which clears it, so skipping it while logging is off changes no output.
		bool Logging()
		{
			const auto* log = spdlog::default_logger_raw();
			return log && log->should_log(spdlog::level::info);
		}
		void Report(std::string_view kind, RE::BGSTextureSet* set)
		{
			if (reported.size() >= 64 || !Logging()) { return; }
			const char* path = Diffuse(set);
			const std::string key = std::string(kind) + ":" + (path ? path : "");
			if (reported.insert(key).second) {
				logger::info("Blood decals B4: {} texture={}", kind, path ? path : "");
			}
		}
		void Place(RE::BSTempEffectSimpleDecal* decal)
		{
			if (!decal->effect3D || !Terrain(decal->avShape.get()) ||
				decal->effect3D->GetGeometryRuntimeData().skinInstance) {
				Report("simple blood on non-terrain or unresolved receiver: native", decal->textureSet);
				return;
			}
			targets.try_emplace(decal->effect3D.get(), decal);
			Report("terrain simple blood geometry identified", decal->textureSet);
		}
		void Place(RE::BSTempEffectGeometryDecal* decal)
		{
			if (!decal->decal || !Terrain(decal->attachedGeometry.get()) ||
				decal->decal->GetGeometryRuntimeData().skinInstance) {
				Report("blood on non-terrain or unresolved receiver: native", decal->texSet);
				return;
			}
			targets.try_emplace(decal->decal.get(), decal);
			Report("terrain blood geometry identified", decal->texSet);
		}
		// Returns true when the effect's textures are blood, so that only Place() decides it.
		bool Classify(RE::BSTempEffect* effect)
		{
			if (auto* decal = netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect)) {
				if (!Blood(decal->textureSet)) {
					Report("unmatched simple decal: native", decal->textureSet);
					return false;
				}
				if (decal->textureSet2 && !Blood(decal->textureSet2)) {
					Report("unmatched secondary simple texture: native", decal->textureSet2);
					return false;
				}
				Place(decal);
				return true;
			}
			auto* decal = netimmerse_cast<RE::BSTempEffectGeometryDecal*>(effect);
			if (!decal) {
				if (reported.size() < 64 && Logging()) {
					const auto* rtti = effect->GetRTTI();
					const std::string key = "type:" + std::string(rtti ? rtti->GetName() : "unknown");
					if (reported.insert(key).second) {
						logger::info("Blood decals B4: other effect class={} type={} kept native", key, static_cast<int>(effect->GetType()));
					}
				}
				return false;
			}
			if (!Blood(decal->texSet)) {
				Report("unmatched geometry decal: native", decal->texSet);
				return false;
			}
			if (decal->texSet2 && !Blood(decal->texSet2)) {
				Report("unmatched secondary geometry texture: native", decal->texSet2);
				return false;
			}
			Place(decal);
			return true;
		}
		void Observe(RE::BSTempEffect* effect)
		{
			if (effect && visited.insert(effect).second) { Classify(effect); }
		}
		Entry Read(RE::BSTempEffect* effect)
		{
			Entry entry{ effect, effect ? effect->GetRTTI() : nullptr };
			if (auto* decal = netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect)) {
				entry.first = decal->textureSet;
				entry.second = decal->textureSet2;
			} else if (auto* geometry = netimmerse_cast<RE::BSTempEffectGeometryDecal*>(effect)) {
				entry.first = geometry->texSet;
				entry.second = geometry->texSet2;
			}
			return entry;
		}
		// Contains() re-observes a node's decals for each decal drawn under it that is not a
		// target yet, which costs the node's decal count squared. When every entry reads the same
		// as at this frame's last full pass, the texture half of Classify() would repeat its answers
		// (texture sets are form data) and its already made reports, so only the blood entries,
		// which their receivers decide, are placed again. Any difference, such as a decal attached
		// since or a reused slot, repeats the full pass.
		void Rescan(RE::BGSDecalNode* node)
		{
			const auto& effects = node->GetRuntimeData().decals;
			auto& scan = scanned[node];
			bool same = scan.entries.size() == effects.size();
			for (std::uint32_t index = 0; same && index < effects.size(); ++index) {
				same = Read(effects[index].get()) == scan.entries[index];
			}
			if (same) {
				for (const auto index : scan.blood) {
					auto* effect = effects[index].get();
					if (auto* decal = netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect)) {
						Place(decal);
					} else if (auto* geometry = netimmerse_cast<RE::BSTempEffectGeometryDecal*>(effect)) {
						Place(geometry);
					}
				}
				return;
			}
			scan.entries.clear();
			scan.blood.clear();
			for (std::uint32_t index = 0; index < effects.size(); ++index) {
				auto* effect = effects[index].get();
				scan.entries.push_back(Read(effect));
				if (effect && Classify(effect)) { scan.blood.push_back(index); }
			}
		}
	}

	void Update()
	{
		targets.clear();
		visited.clear();
		scanned.clear();
		if (!Settings::enableBloodDecals || !Settings::enableTessellation || !Settings::useClipmap) { return; }
		if (!reportedStart) {
			reportedStart = true;
			logger::info("Blood decals B4 enabled: direct lists and attached nodes, prefixes={}", Settings::bloodDecalTexturePrefixes);
		}
		auto* manager = RE::BGSDecalManager::GetSingleton();
		if (!manager) {
			if (reported.insert("no-manager").second) { logger::info("Blood decals B4: decal manager unavailable"); }
			return;
		}
		for (const auto& effect : manager->decals) {
			Observe(effect.get());
		}
		for (const auto& decal : manager->simpleDecals) {
			Observe(decal.get());
		}
		size_t attached = 0;
		for (const auto& node : manager->decalNodes) {
			if (!node) { continue; }
			const auto& effects = node->GetRuntimeData().decals;
			attached += effects.size();
			for (const auto& effect : effects) { Observe(effect.get()); }
		}
		const std::array<size_t, 6> counts{ manager->decals.size(), manager->simpleDecals.size(),
			manager->decalNodes.size(), attached, visited.size(), targets.size() };
		const auto now = std::chrono::steady_clock::now();
		if (now >= nextCensus && (counts != lastCounts || nextCensus.time_since_epoch().count() == 0)) {
			logger::info("Blood decals B4 census: direct={} simple={} nodes={} attached={} unique={} terrainTargets={}",
				counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
			lastCounts = counts;
			nextCensus = now + std::chrono::seconds(5);
		}
	}

	bool Contains(RE::BSGeometry* geometry)
	{
		if (!Settings::enableBloodDecals || !geometry) { return false; }
		if (targets.contains(geometry)) { return true; }
		auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property || !property->flags.any(Flag::kDecal, Flag::kDynamicDecal)) { return false; }

		auto* parent = geometry->parent;
		for (unsigned depth = 0; parent && depth < 8; ++depth, parent = parent->parent) {
			if (auto* node = netimmerse_cast<RE::BGSDecalNode*>(parent)) {
				Rescan(node);
				break;
			}
		}
		if (targets.contains(geometry)) { return true; }

		if (Settings::logDraws && reportedDrawTextures.size() < 32) {
			if (auto* texture = property->GetBaseTexture()) {
				const std::string path = texture->name.c_str();
				if (reportedDrawTextures.insert("draw:" + path).second) {
					logger::info("Blood decals B4: unregistered decal draw, texture={} prefixMatch={}; kept native",
						path, BloodDecalFilter::Matches(path, Settings::bloodDecalTexturePrefixes));
				}
			}
		}
		return false;
	}
	void NoteDraw(bool routed)
	{
		auto& once = routed ? reportedDraw : reportedSkip;
		if (!once) {
			once = true;
			logger::info("Blood decals B4: {}", routed ? "first terrain blood draw displaced" :
				"blood draw kept native: shader pending or unsupported pipeline");
		}
	}
	void Reset()
	{
		targets.clear(); reported.clear(); reportedDrawTextures.clear();
		visited.clear(); scanned.clear(); lastCounts = {}; nextCensus = {};
		reportedDraw = reportedSkip = reportedStart = false;
	}
}
