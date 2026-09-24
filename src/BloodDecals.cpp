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
		// And the diffuse paths of those texture sets, kept apart so that passes which do not
		// compare them do not read them. The paths are pooled strings, so a path assigned at run
		// time (as SKSE's TextureSet.SetNthTexturePath can) reads as a new pointer. Only the
		// pointer is kept: if the string read at a full pass is freed and a different one made at
		// its address before the next comparison (one slot assigned twice in between), the change
		// goes unnoticed and that pass's answer stands until the list changes or a reload.
		using Paths = std::array<const char*, 2>;
		struct Scan
		{
			std::vector<Entry> entries;
			std::vector<Paths> paths;
			std::vector<std::uint32_t> blood;
			std::uint64_t listed{};  // the last frame Update() passed this node
			std::uint64_t drawn{};   // the last frame Contains() passed it
		};
		std::unordered_map<RE::BGSDecalNode*, Scan> scanned;
		Scan direct, simple;  // the manager's own lists
		std::uint64_t frame{};
		void Forget()
		{
			scanned.clear();
			direct = {};
			simple = {};
		}
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
		Paths PathsOf(const Entry& entry)
		{
			return { Diffuse(entry.first), Diffuse(entry.second) };
		}
		// Update() classifies every listed decal each frame, and Contains() a node's decals for
		// each decal drawn under it that is not a target yet, which costs the node's decal count
		// squared. When every entry reads the same as at the list's last full pass, the texture
		// half of Classify() would repeat its answers, and its reports, made or refused (at the
		// cap or with logging off) at that pass, would be refused again until a reload's Reset()
		// forgets the scans. So only the blood entries, which their receivers decide, are placed
		// again. Any difference, such as a decal attached since or a reused slot, repeats the full
		// pass. Without `paths`, a path rewritten since that pass goes unnoticed.
		template <class List>
		void Rescan(Scan& scan, const List& effects, bool paths)
		{
			bool same = scan.entries.size() == effects.size();
			for (std::uint32_t index = 0; same && index < effects.size(); ++index) {
				const Entry entry = Read(effects[index].get());
				same = entry == scan.entries[index] && (!paths || PathsOf(entry) == scan.paths[index]);
			}
			if (same) {
				for (const auto index : scan.blood) {
					RE::BSTempEffect* effect = effects[index].get();
					if (auto* decal = netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect)) {
						Place(decal);
					} else if (auto* geometry = netimmerse_cast<RE::BSTempEffectGeometryDecal*>(effect)) {
						Place(geometry);
					}
				}
				return;
			}
			scan.entries.clear();
			scan.paths.clear();
			scan.blood.clear();
			for (std::uint32_t index = 0; index < effects.size(); ++index) {
				RE::BSTempEffect* effect = effects[index].get();
				scan.entries.push_back(Read(effect));
				scan.paths.push_back(PathsOf(scan.entries.back()));
				if (effect && Classify(effect)) { scan.blood.push_back(index); }
			}
		}
		// A node's first pass from Contains() in a frame used to be a full one, so it compares the
		// paths as well; later passes in the frame compare what they compared before.
		void Rescan(RE::BGSDecalNode* node)
		{
			auto& scan = scanned[node];
			const bool first = scan.drawn != frame;
			scan.drawn = frame;
			Rescan(scan, node->GetRuntimeData().decals, first);
		}
	}

	void Update()
	{
		targets.clear();
		visited.clear();
		++frame;
		// New prefixes come with a reload, whose Reset() forgets the scans as well.
		if (prefixSource != Settings::bloodDecalTexturePrefixes) { Forget(); }
		if (!Settings::enableBloodDecals || !Settings::enableTessellation || !Settings::useClipmap) {
			// Contains() can still rescan nodes; those scans last one frame, as before.
			Forget();
			return;
		}
		if (!reportedStart) {
			reportedStart = true;
			logger::info("Blood decals B4 enabled: direct lists and attached nodes, prefixes={}", Settings::bloodDecalTexturePrefixes);
		}
		auto* manager = RE::BGSDecalManager::GetSingleton();
		if (!manager) {
			if (reported.insert("no-manager").second) { logger::info("Blood decals B4: decal manager unavailable"); }
			return;
		}
		// In the order the effects were observed before. An effect listed twice is classified
		// twice, which places and reports nothing new.
		Rescan(direct, manager->decals, true);
		Rescan(simple, manager->simpleDecals, true);
		size_t attached = 0;
		for (const auto& node : manager->decalNodes) {
			if (!node) { continue; }
			const auto& effects = node->GetRuntimeData().decals;
			attached += effects.size();
			auto& scan = scanned[node.get()];
			scan.listed = frame;
			Rescan(scan, effects, true);
		}
		// A node neither listed nor drawn since the last frame starts again from a full pass.
		std::erase_if(scanned, [](const auto& item) {
			return std::max(item.second.listed, item.second.drawn) + 1 < frame;
		});
		// Only the census reads the count of distinct effects.
		if (Logging()) {
			for (const auto& effect : manager->decals) { if (effect) { visited.insert(effect.get()); } }
			for (const auto& decal : manager->simpleDecals) { if (decal) { visited.insert(decal.get()); } }
			for (const auto& node : manager->decalNodes) {
				if (!node) { continue; }
				for (const auto& effect : node->GetRuntimeData().decals) {
					if (effect) { visited.insert(effect.get()); }
				}
			}
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

	bool MaybeTarget(RE::BSGeometry* geometry)
	{
		if (!Settings::enableBloodDecals || !geometry) { return false; }
		auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		return (property && property->flags.any(Flag::kDecal, Flag::kDynamicDecal)) ||
			(!targets.empty() && targets.contains(geometry));
	}

	bool Contains(RE::BSGeometry* geometry)
	{
		// The draw hooks skip every draw MaybeTarget() turns down, so a new way for a draw to
		// become a target has to be added there, not here.
		if (!MaybeTarget(geometry)) { return false; }
		if (targets.contains(geometry)) { return true; }
		auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();

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
		visited.clear(); Forget(); lastCounts = {}; nextCensus = {};
		reportedDraw = reportedSkip = reportedStart = false;
	}
}
