// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ActorShapes.h"
#include "Clipmap.h"

#include "ClipmapUpdateCS.h"
#include "Globals.h"
#include "HeatSources.h"
#include "MagicImpacts.h"
#include "ObjectStamps.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Tessellation.h"
#include "Weather.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>

namespace Clipmap
{
	namespace
	{

		struct WindowCB
		{
			float centreAndFade[4]{};

			float raise[4]{};

			float window1[4]{};

			// x = internal viewport height / 2 / TessellationScreenPixels (0 = cap off or not
			// measured yet).
			float screen[4]{};
		};
		static_assert(sizeof(WindowCB) % 16 == 0);
		static_assert(offsetof(WindowCB, screen) == 48, "the generated ClipmapWindow reads Screen at c3");

		struct ParamsCB
		{
			float window[4]{};
			float control[4]{};
			float weather[4]{};
			float stamps[kMaxStamps][4]{};
			float stampParams[kMaxStamps][4]{};
			float stampShape[kMaxStamps][4]{};

			float stampMotion[kMaxStamps][4]{};

			float coarse[4]{};

			float rimShape[4]{};
			float snowRim[4]{};

			float raise[4]{};

			float raiseWindow[4]{};

			float stampBounds[kMaxStamps][4]{};

			uint32_t noNoiseMask[4]{};

			uint32_t activityShape[4]{};
		};
		static_assert(sizeof(ParamsCB) % 16 == 0);

		ID3D11Texture2D*           g_texture[kMaxLevels]{};
		ID3D11ShaderResourceView*  g_srv[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_uav[kMaxLevels]{};

		ID3D11Texture2D*           g_decayTexture[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_decayUAV[kMaxLevels]{};

		ID3D11ShaderResourceView*  g_decaySRV[kMaxLevels]{};

		ID3D11Texture2D*           g_activity[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_activityUAV[kMaxLevels]{};
		ID3D11ShaderResourceView*  g_activitySRV[kMaxLevels]{};

		// GroupLive flags, one per update group. An update reads [parity] (through the
		// group list pass) and writes [parity ^ 1], then the parity flips. Known once an
		// update has written every flag of the level.
		ID3D11Texture2D*           g_live[kMaxLevels][2]{};
		ID3D11UnorderedAccessView* g_liveUAV[kMaxLevels][2]{};
		ID3D11ShaderResourceView*  g_liveSRV[kMaxLevels][2]{};
		uint32_t                   g_liveParity[kMaxLevels]{};
		bool                       g_liveKnown[kMaxLevels]{};

		// The group list, its count and the DispatchIndirect arguments, shared by the
		// levels, which update one after the other.
		ID3D11ComputeShader*       g_groupListCS{ nullptr };
		ID3D11Buffer*              g_groupList{ nullptr };
		ID3D11UnorderedAccessView* g_groupListUAV{ nullptr };
		ID3D11ShaderResourceView*  g_groupListSRV{ nullptr };
		ID3D11Buffer*              g_groupCount{ nullptr };
		ID3D11UnorderedAccessView* g_groupCountUAV{ nullptr };
		ID3D11ShaderResourceView*  g_groupCountSRV{ nullptr };
		ID3D11Buffer*              g_groupArgs{ nullptr };
		ID3D11UnorderedAccessView* g_groupArgsUAV{ nullptr };

		ID3D11SamplerState*        g_sampler{ nullptr };
		ID3D11ComputeShader*       g_updateCS{ nullptr };
		ID3D11ComputeShader*       g_updateListedCS{ nullptr };
		ID3D11Buffer*              g_paramsCB{ nullptr };
		ID3D11Buffer*              g_windowCB{ nullptr };
		bool                       g_failed{ false };

		int32_t g_prevWindowX[kMaxLevels]{};
		int32_t g_prevWindowY[kMaxLevels]{};
		bool    g_prevWindowValid[kMaxLevels]{};

		float g_windowCentreX{ 0.0f };
		float g_windowCentreY{ 0.0f };
		float g_windowHalfExtent{ 0.0f };
		bool  g_windowValid{ false };

		// Slots the update and group list passes bind: u0-u3 and t0-t6.
		constexpr UINT kComputeUAVs = 4;
		constexpr UINT kComputeSRVs = 7;

		struct ComputeStageGuard
		{
			explicit ComputeStageGuard(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_context->CSGetShader(&_shader, nullptr, nullptr);
				_context->CSGetConstantBuffers(0, 1, &_cb);
				_context->CSGetUnorderedAccessViews(0, kComputeUAVs, _uav);
				_context->CSGetShaderResources(0, kComputeSRVs, _srv);
				_context->CSGetSamplers(0, 1, &_sampler);
			}

			~ComputeStageGuard()
			{
				const UINT noOffset[kComputeUAVs] = { static_cast<UINT>(-1), static_cast<UINT>(-1),
					static_cast<UINT>(-1), static_cast<UINT>(-1) };
				_context->CSSetShader(_shader, nullptr, 0);
				_context->CSSetConstantBuffers(0, 1, &_cb);
				_context->CSSetUnorderedAccessViews(0, kComputeUAVs, _uav, noOffset);
				_context->CSSetShaderResources(0, kComputeSRVs, _srv);
				_context->CSSetSamplers(0, 1, &_sampler);

				if (_shader) {
					_shader->Release();
				}
				if (_cb) {
					_cb->Release();
				}
				for (auto*& srv : _srv) {
					if (srv) {
						srv->Release();
					}
				}
				if (_sampler) {
					_sampler->Release();
				}
				for (auto*& uav : _uav) {
					if (uav) {
						uav->Release();
					}
				}
			}

			ComputeStageGuard(const ComputeStageGuard&) = delete;
			ComputeStageGuard& operator=(const ComputeStageGuard&) = delete;

		private:
			ID3D11DeviceContext*       _context;

			ID3D11ShaderResourceView*  _srv[kComputeSRVs]{};
			ID3D11SamplerState*        _sampler{ nullptr };
			ID3D11ComputeShader*       _shader{ nullptr };
			ID3D11Buffer*              _cb{ nullptr };
			ID3D11UnorderedAccessView* _uav[kComputeUAVs]{};
		};

		bool CreateLevel(ID3D11Device* a_device, uint32_t a_level)
		{
			if (a_level >= kMaxLevels) {
				return false;
			}
			if (g_srv[a_level] && g_uav[a_level] && g_decayUAV[a_level] && g_decaySRV[a_level] &&
				g_activityUAV[a_level] && g_activitySRV[a_level]) {
				return true;
			}

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTexels;
			desc.Height = kTexels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			const std::vector<float> zeros(static_cast<size_t>(kTexels) * kTexels, 0.0f);
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = zeros.data();
			initial.SysMemPitch = kTexels * sizeof(float);

			if (FAILED(a_device->CreateTexture2D(&desc, &initial, &g_texture[a_level]))) {
				logger::error("Clipmap: CreateTexture2D failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_texture[a_level], nullptr, &g_srv[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_texture[a_level], nullptr, &g_uav[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView failed for level {}", a_level);
				return false;
			}

			D3D11_TEXTURE2D_DESC decayDesc = desc;
			decayDesc.Format = DXGI_FORMAT_R8G8_UNORM;

			decayDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

			const std::vector<uint8_t> decayZeros(static_cast<size_t>(kTexels) * kTexels * 2, 0);
			D3D11_SUBRESOURCE_DATA decayInitial{};
			decayInitial.pSysMem = decayZeros.data();
			decayInitial.SysMemPitch = kTexels * 2 * sizeof(uint8_t);

			if (FAILED(a_device->CreateTexture2D(
					&decayDesc, &decayInitial, &g_decayTexture[a_level]))) {
				logger::error("Clipmap: CreateTexture2D (decay) failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_decayTexture[a_level], nullptr, &g_decayUAV[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView (decay) failed for level {}",
					a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_decayTexture[a_level], nullptr, &g_decaySRV[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView (decay) failed for level {}",
					a_level);
				return false;
			}

			D3D11_TEXTURE2D_DESC activityDesc{};
			activityDesc.Width = ActivityTexelsFor(a_level);
			activityDesc.Height = ActivityTexelsFor(a_level);
			activityDesc.MipLevels = 1;
			activityDesc.ArraySize = 1;
			activityDesc.Format = DXGI_FORMAT_R32_UINT;
			activityDesc.SampleDesc.Count = 1;
			activityDesc.Usage = D3D11_USAGE_DEFAULT;
			activityDesc.BindFlags =
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			// Zero like the field, so it holds the field's max |h| from the start.
			const std::vector<uint32_t> activityZeros(
				static_cast<size_t>(activityDesc.Width) * activityDesc.Height, 0);
			D3D11_SUBRESOURCE_DATA activityInitial{};
			activityInitial.pSysMem = activityZeros.data();
			activityInitial.SysMemPitch = activityDesc.Width * sizeof(uint32_t);

			if (FAILED(a_device->CreateTexture2D(&activityDesc, &activityInitial, &g_activity[a_level]))) {
				logger::error("Clipmap: CreateTexture2D (activity) failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_activity[a_level], nullptr, &g_activityUAV[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView (activity) failed for "
							  "level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_activity[a_level], nullptr, &g_activitySRV[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView (activity) failed for "
							  "level {}", a_level);
				return false;
			}

			// Zero, like the field. The first update still runs every group and writes
			// every flag.
			D3D11_TEXTURE2D_DESC liveDesc = activityDesc;
			liveDesc.Width = kGroups;
			liveDesc.Height = kGroups;

			const std::vector<uint32_t> liveZeros(static_cast<size_t>(kGroups) * kGroups, 0);
			D3D11_SUBRESOURCE_DATA      liveInitial{};
			liveInitial.pSysMem = liveZeros.data();
			liveInitial.SysMemPitch = kGroups * sizeof(uint32_t);

			for (uint32_t i = 0; i < 2; ++i) {
				if (FAILED(a_device->CreateTexture2D(&liveDesc, &liveInitial, &g_live[a_level][i])) ||
					FAILED(a_device->CreateUnorderedAccessView(
						g_live[a_level][i], nullptr, &g_liveUAV[a_level][i])) ||
					FAILED(a_device->CreateShaderResourceView(
						g_live[a_level][i], nullptr, &g_liveSRV[a_level][i]))) {
					logger::error("Clipmap: group flags unavailable for level {}", a_level);
					return false;
				}
			}
			g_liveParity[a_level] = 0;
			g_liveKnown[a_level] = false;

			logger::info("Clipmap level {}: {}x{} texels over {:.0f} world units "
						 "({:.2f} per cell, +/- {:.1f} m)",
				a_level, kTexels, kTexels, WorldSizeFor(a_level), CellSizeFor(a_level),
				WorldSizeFor(a_level) * 0.5f / 70.0f);
			return true;
		}

		bool CompileComputeShader(ID3D11Device* a_device, const std::string& a_source,
			const char* a_name, ID3D11ComputeShader** a_out, bool a_groupList = false)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;

			const std::string maxStamps = std::to_string(kMaxStamps);
			const D3D_SHADER_MACRO defines[] = {
				{ "MAX_STAMPS", maxStamps.c_str() },
				{ a_groupList ? "GROUP_LIST" : nullptr, "1" },  // a null name ends the list
				{ nullptr, nullptr }
			};

			const HRESULT hr = ::D3DCompile(
				a_source.c_str(), a_source.size(), a_name, defines, nullptr,
				"main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

			if (FAILED(hr)) {
				logger::error("{} failed to compile: {}", a_name,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}

			const HRESULT createHr = a_device->CreateComputeShader(
				code->GetBufferPointer(), code->GetBufferSize(), nullptr, a_out);
			code->Release();

			if (FAILED(createHr)) {
				logger::error("Clipmap: CreateComputeShader failed for {}", a_name);
				return false;
			}
			return true;
		}

		// The buffers of the group list pass. The count and the arguments take raw
		// views so the pass can InterlockedAdd into them; the update reads the count
		// through its own raw view, and the arguments buffer is only ever a UAV and the
		// DispatchIndirect source. All of it is Direct3D 11.0 core.
		bool CreateGroupList(ID3D11Device* a_device)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kGroups * kGroups * sizeof(uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(uint32_t);

			if (FAILED(a_device->CreateBuffer(&desc, nullptr, &g_groupList)) ||
				FAILED(a_device->CreateUnorderedAccessView(g_groupList, nullptr, &g_groupListUAV)) ||
				FAILED(a_device->CreateShaderResourceView(g_groupList, nullptr, &g_groupListSRV))) {
				return false;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC rawUAV{};
			rawUAV.Format = DXGI_FORMAT_R32_TYPELESS;
			rawUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			rawUAV.Buffer.NumElements = 4;
			rawUAV.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

			D3D11_SHADER_RESOURCE_VIEW_DESC rawSRV{};
			rawSRV.Format = DXGI_FORMAT_R32_TYPELESS;
			rawSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			rawSRV.BufferEx.NumElements = 4;
			rawSRV.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;

			desc.ByteWidth = 4 * sizeof(uint32_t);
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			desc.StructureByteStride = 0;

			if (FAILED(a_device->CreateBuffer(&desc, nullptr, &g_groupCount)) ||
				FAILED(a_device->CreateUnorderedAccessView(g_groupCount, &rawUAV, &g_groupCountUAV)) ||
				FAILED(a_device->CreateShaderResourceView(g_groupCount, &rawSRV, &g_groupCountSRV))) {
				return false;
			}

			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS |
				D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

			if (FAILED(a_device->CreateBuffer(&desc, nullptr, &g_groupArgs)) ||
				FAILED(a_device->CreateUnorderedAccessView(g_groupArgs, &rawUAV, &g_groupArgsUAV))) {
				return false;
			}

			return CompileComputeShader(a_device, kGroupListShader, "ClipmapGroupListCS",
				&g_groupListCS);
		}

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

		std::atomic<uint32_t> g_surfacesLogged{ 0 };

		std::atomic<uint32_t> g_sizesLogged{ 0 };
		constexpr uint32_t    kMaxSizesLogged = 4;

		void LogStampSize(const Stamp& a_stamp)
		{
			if (!Settings::logStampShape || a_stamp.kind != Stamp::Kind::kPrint) {
				return;
			}
			if (g_sizesLogged.load() >= kMaxSizesLogged ||
				g_sizesLogged.fetch_add(1) >= kMaxSizesLogged) {
				return;
			}

			const float length = a_stamp.radius * 2.0f;
			const float width = a_stamp.halfWidth * 2.0f;

			const float vertexSpacing = std::max(Settings::tessellationTargetSpacing, 0.01f);

			logger::info("Print size: {:.1f} x {:.1f} world units | {:.0f} x {:.0f} field "
						 "texels at {:.2f}/cell | about {:.1f} x {:.1f} GENERATED VERTICES "
						 "at spacing {:.1f}",
				length, width, length / kCellSize, width / kCellSize, kCellSize,
				length / vertexSpacing, width / vertexSpacing,
				Settings::tessellationTargetSpacing);

			if (length / vertexSpacing < 8.0f) {
				logger::warn("  that is too few vertices to carry a silhouette - the mark "
							 "will read as a smooth blob whatever the mask contains. The "
							 "field holds {:.0f}x more detail than the geometry samples. "
							 "Lower TessellationTargetSpacing (and raise "
							 "TessellationMaxFactor with it, or the cap just clamps it "
							 "back).",
					vertexSpacing / kCellSize);
			}
		}

		std::atomic<uint32_t> g_feetLogged{ 0 };
		constexpr uint32_t    kMaxFeetLogged = 40;

		void LogFootShape(float a_side, float a_radius, float a_z, float a_landZ, bool a_isFoot)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (g_feetLogged.load() >= kMaxFeetLogged ||
				g_feetLogged.fetch_add(1) >= kMaxFeetLogged) {
				return;
			}

			logger::info("Foot test: side {:+7.2f} vs separation {:.2f} -> {:<6} | "
						 "shape radius {:5.2f} | bottom {:+7.2f} above land",
				a_side, Settings::stampFootSeparation, a_isFoot ? "PRINT" : "circle",
				a_radius, (a_z - a_radius) - a_landZ);
		}

		void LogStampSurface(const Surfaces::Ground& a_ground, const RE::NiPoint3& a_position)
		{
			if (!Settings::logStampSurfaces) {
				return;
			}

			const auto index = a_ground.profile >= 0 ?
								   static_cast<uint32_t>(a_ground.profile) +
									   static_cast<uint32_t>(Surfaces::Type::kCount) :
								   static_cast<uint32_t>(a_ground.type);
			const uint32_t bit = 1u << (index < 32u ? index : static_cast<uint32_t>(a_ground.type));
			if (g_surfacesLogged.fetch_or(bit) & bit) {
				return;
			}

			const auto* profile = Surfaces::ProfileName(a_ground.profile);
			const auto& response = a_ground.response;
			logger::info(
				"Stamp surface at ({:.0f}, {:.0f}): {:<8} depth x{:.2f} radius x{:.2f} "
				"shoulder {:.2f} decay x{:.2f}{}",
				a_position.x, a_position.y, Surfaces::Name(a_ground.type),
				response.depthScale, response.radiusScale, response.shoulder,
				response.decayScale,
				profile ? std::format("  <- profile [{}]", profile) :
						  std::string{ "  (no profile - keyword list or material id)" });
		}

		struct TrackedActor
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };
		};

		std::unordered_map<RE::FormID, TrackedActor> g_actorMotion;
		uint64_t                                     g_actorFrame{ 0 };

		constexpr float kMaxActorMotion = 48.0f;

		void TrackActorMotion(RE::FormID a_form, const RE::NiPoint3& a_position,
			float& a_motionX, float& a_motionY)
		{
			a_motionX = 0.0f;
			a_motionY = 0.0f;

			const auto previous = g_actorMotion.find(a_form);
			if (previous != g_actorMotion.end()) {
				const float dx = a_position.x - previous->second.x;
				const float dy = a_position.y - previous->second.y;
				if (dx * dx + dy * dy <= kMaxActorMotion * kMaxActorMotion) {
					a_motionX = dx;
					a_motionY = dy;
				}
			}

			g_actorMotion[a_form] = { a_position.x, a_position.y, g_actorFrame };
		}

		void AppendActorStamps(RE::Actor* a_actor, const RE::NiPoint3& a_eye,
			float a_maxDistanceSq, std::vector<Stamp>& a_out)
		{
			if (!a_actor || a_out.size() >= kMaxStamps) {
				return;
			}

			if (a_actor->IsOnMount()) {
				return;
			}

			auto* root = a_actor->Get3D(false);
			if (!root) {
				return;
			}

			const RE::NiPoint3 position = a_actor->GetPosition();
			if (a_eye.GetSquaredDistance(position) > a_maxDistanceSq) {
				return;
			}

			if (Clipmap::Stamp torch{}; HeatSources::ForActor(a_actor, torch)) {
				a_out.push_back(torch);
				if (a_out.size() >= kMaxStamps) {
					return;
				}
			}

			const auto  ground = Surfaces::GroundAt(position);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			LogStampSurface(ground, position);

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				return;
			}

			const float feetZ = position.z;
			const float reach = Settings::stampFootReach;

			const float  heading = a_actor->GetAngleZ();
			const float  headingSin = std::sin(heading);
			const float  headingCos = std::cos(heading);
			const RE::NiPoint3 forward{ headingSin, headingCos, 0.0f };
			const RE::NiPoint3 right{ headingCos, -headingSin, 0.0f };

			float motionX = 0.0f;
			float motionY = 0.0f;
			TrackActorMotion(a_actor->GetFormID(), position, motionX, motionY);

			if (SnowSparkle::Enabled() && !a_actor->IsDead()) {
				RE::NiPoint3 velocity{};
				a_actor->GetLinearVelocity(velocity);

				SnowSparkle::NoteContact(
					surface, position, forward.x, forward.y, velocity.x, velocity.y, 18.0f);
			}

			const bool booted =
				a_actor->GetRace() &&
				a_actor->GetRace()->HasKeywordString(Settings::stampShapeKeyword);

			const bool printed =
				Settings::enableStampShapes && StampShapes::Ready() &&
				response.print >= 0.5f && booted;

			const bool oriented = Settings::stampFootShape && booted;

			auto* const tes = RE::TES::GetSingleton();

			const float clearance =
				Settings::stampGroundClearance * std::max(response.clearanceScale, 0.0f);

			ActorShapes::WalkCollision(
				root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
					if (a_out.size() >= kMaxStamps) {
						return RE::BSVisit::BSVisitControl::kStop;
					}

					RE::NiPoint3 centre;
					float        radius = 0.0f;
					if (!ActorShapes::GetBound(a_object, centre, radius) || radius <= 0.0f) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}

					if (centre.z - radius > feetZ + reach) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}

					if (clearance > 0.0f && tes) {
						float landZ = 0.0f;
						if (tes->GetLandHeight(centre, landZ) &&
							(centre.z - radius) - landZ > clearance) {
							return RE::BSVisit::BSVisitControl::kContinue;
						}
					}

					Stamp stamp{};
					stamp.snow = surface == Surfaces::Type::kSnow;
					stamp.x = centre.x;
					stamp.y = centre.y;
					stamp.radius = radius * Settings::stampRadiusScale * response.radiusScale;

					if (printed || oriented) {
						const RE::NiPoint3 offset{ centre.x - position.x,
							centre.y - position.y, 0.0f };
						const float side = offset.Dot(right);
						const bool  isFoot =
							std::abs(side) >= Settings::stampFootSeparation;

						if (Settings::logStampFeet) {
							float reportLand = 0.0f;
							if (tes) {
								tes->GetLandHeight(centre, reportLand);
							}
							LogFootShape(side, radius, centre.z, reportLand, isFoot);
						}

						if (isFoot) {

							if (printed) {
								stamp.kind = Stamp::Kind::kPrint;
								stamp.mirror = side < 0.0f ? -1.0f : 1.0f;
							}

							stamp.forwardX = forward.x;
							stamp.forwardY = forward.y;

							stamp.radius *= Settings::stampFootLength;
							stamp.halfWidth = stamp.radius * Settings::stampFootAspect;
						}
					}

					const float ordinary =
						Settings::stampDepth * response.depthScale * Weather::DepthScale();

					stamp.depth = Surfaces::MarkDepth(surface, ordinary, 1.0f, stamp.x, stamp.y);
					stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);

					stamp.decay = std::clamp(
						Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(),
						0.0f, 0.9999f);

					stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);

					if (stamp.kind != Stamp::Kind::kPrint) {
						stamp.motionX = motionX;
						stamp.motionY = motionY;
					}

					LogStampSize(stamp);
					a_out.push_back(stamp);

					return RE::BSVisit::BSVisitControl::kContinue;
				});
		}

		std::vector<Stamp> GatherStamps(float a_deltaSeconds)
		{
			++g_actorFrame;

			for (auto it = g_actorMotion.begin(); it != g_actorMotion.end();) {
				it = (it->second.frame + 120 < g_actorFrame) ? g_actorMotion.erase(it) :
															   std::next(it);
			}

			std::vector<Stamp> stamps;
			stamps.reserve(kMaxStamps);

			RE::NiPoint3 anchor{};
			if (auto* player = globals::game::player) {
				anchor = player->GetPosition();
			}

			std::vector<RE::ActorPtr> actors;
			MagicImpacts::Append(anchor, stamps);
			CollectActors(actors);
			if (actors.empty()) {

				ObjectStamps::Append(a_deltaSeconds, anchor, stamps);
				return stamps;
			}

			const float maxDistance = kWorldSize * 0.375f;
			const float maxDistanceSq = maxDistance * maxDistance;

			std::sort(actors.begin(), actors.end(),
				[&anchor](const RE::ActorPtr& a_lhs, const RE::ActorPtr& a_rhs) {
					return anchor.GetSquaredDistance(a_lhs->GetPosition()) <
					       anchor.GetSquaredDistance(a_rhs->GetPosition());
				});

			for (const auto& actor : actors) {
				AppendActorStamps(actor.get(), anchor, maxDistanceSq, stamps);
				if (stamps.size() >= kMaxStamps) {
					break;
				}
			}

			ObjectStamps::Append(a_deltaSeconds, anchor, stamps);

			return stamps;
		}
	}

	void ResetDiagnostics()
	{
		g_sizesLogged.store(0);
		g_surfacesLogged.store(0);
		g_feetLogged.store(0);
	}

	bool Ready()
	{
		return g_srv[0] && g_uav[0] && g_sampler && g_updateCS && g_paramsCB && g_windowCB;
	}

	bool Initialize()
	{
		if (Ready()) {
			return true;
		}
		if (g_failed || !globals::Ready()) {
			return false;
		}

		auto* device = globals::d3d::device;

		const auto fail = [&](const char* a_what) {
			logger::error("Clipmap: {}", a_what);
			Release();
			g_failed = true;
			return false;
		};

		for (uint32_t level = 0; level < LevelCount(); ++level) {
			if (!CreateLevel(device, level)) {
				return fail("level allocation failed");
			}
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return fail("CreateSamplerState failed");
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(ParamsCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_paramsCB))) {
			return fail("CreateBuffer failed");
		}

		cbDesc.ByteWidth = sizeof(WindowCB);
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_windowCB))) {
			return fail("CreateBuffer (window) failed");
		}

		const std::string update = UpdateShaderSource();
		if (!CompileComputeShader(device, update, "ClipmapUpdateCS", &g_updateCS) ||
			!CompileComputeShader(device, update, "ClipmapUpdateCS (group list)", &g_updateListedCS, true)) {
			return fail("update shader unavailable");
		}

		if (!CreateGroupList(device)) {
			return fail("group list unavailable");
		}

		logger::info("Clipmap ready: {} level(s), marks survive to {:.0f} world units "
					 "({:.1f} m) from the player",
			LevelCount(), WorldSizeFor(LevelCount() - 1) * 0.5f,
			WorldSizeFor(LevelCount() - 1) * 0.5f / 70.0f);
		return true;
	}

	void Release()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};

		drop(g_windowCB);
		drop(g_paramsCB);
		drop(g_updateCS);
		drop(g_updateListedCS);
		drop(g_sampler);

		drop(g_groupListCS);
		drop(g_groupArgsUAV);
		drop(g_groupArgs);
		drop(g_groupCountSRV);
		drop(g_groupCountUAV);
		drop(g_groupCount);
		drop(g_groupListSRV);
		drop(g_groupListUAV);
		drop(g_groupList);

		for (uint32_t level = 0; level < kMaxLevels; ++level) {
			g_prevWindowValid[level] = false;
			g_liveKnown[level] = false;
			for (uint32_t i = 0; i < 2; ++i) {
				drop(g_liveSRV[level][i]);
				drop(g_liveUAV[level][i]);
				drop(g_live[level][i]);
			}
			drop(g_activitySRV[level]);
			drop(g_activityUAV[level]);
			drop(g_activity[level]);
			drop(g_decaySRV[level]);
			drop(g_decayUAV[level]);
			drop(g_decayTexture[level]);
			drop(g_uav[level]);
			drop(g_srv[level]);
			drop(g_texture[level]);
		}
	}

	void Update(float a_deltaSeconds)
	{
		if (!Ready() || !globals::game::player) {
			return;
		}

		const auto position = globals::game::player->GetPosition();

		const uint32_t levels = LevelCount();
		for (uint32_t level = 0; level < levels; ++level) {
			if (!g_srv[level] && !CreateLevel(globals::d3d::device, level)) {
				logger::error("Clipmap: level {} unavailable, running at {} level(s)",
					level, level);
				break;
			}
		}

		ParamsCB params{};

		params.window[3] = static_cast<float>(kTexels);

		const float dt = std::clamp(a_deltaSeconds, 0.0f, 0.25f);
		params.control[0] = dt;

		const int64_t gatherStart = Profiler::Ticks();
		const auto    stamps = GatherStamps(a_deltaSeconds);
		Profiler::AddCpuTicks(Profiler::CpuScope::kGatherStamps, Profiler::Ticks() - gatherStart);
		const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(stamps.size()), kMaxStamps);
		params.control[1] = static_cast<float>(count);

		static uint32_t reportedCount = 0xFFFFFFFFu;
		if (count != reportedCount) {
			reportedCount = count;
			if (stamps.size() > kMaxStamps) {
				logger::warn("Stamps: {} pressed, {} DROPPED - the frame wanted more than "
				             "the budget of {}, and the furthest were cut",
					count, stamps.size() - kMaxStamps, kMaxStamps);
			} else if (Settings::logObjectStamps) {

				logger::info("Stamps: {} pressed", count);
			}
		}

		params.control[2] = static_cast<float>(kTexels / 2 - 2);

		params.control[3] = std::max(Settings::stampRimSpan, 0.0f);

		params.weather[0] = Weather::FillPerSecond();

		params.weather[1] = Settings::stampSlopeLimit > 0.0f ?
								std::tan(std::clamp(Settings::stampSlopeLimit, 1.0f, 89.0f) *
									0.017453292f) :
								0.0f;
		params.weather[2] = std::clamp(Settings::stampReposeRate, 0.0f, 1.0f);
		params.weather[3] = std::clamp(Settings::stampRimNoise, 0.0f, 8.0f);
		params.rimShape[0] = std::clamp(Settings::stampRimLean, 0.0f, 1.0f);
		params.rimShape[1] = std::clamp(Settings::stampChurn, 0.0f, 8.0f);
		const auto snowValue = [](float overrideValue, float globalValue) {
			return overrideValue < 0.0f ? globalValue : overrideValue;
		};
		params.rimShape[2] = snowValue(Settings::snowStampReposeRate, params.weather[2]);
		params.snowRim[0] = snowValue(Settings::snowStampRimSpan, params.control[3]);
		params.snowRim[1] = snowValue(Settings::snowStampRimNoise, params.weather[3]);
		params.snowRim[2] = snowValue(Settings::snowStampRimLean, params.rimShape[0]);
		params.snowRim[3] = snowValue(Settings::snowStampChurn, params.rimShape[1]);

		for (uint32_t i = 0; i < count; ++i) {
			params.stamps[i][0] = stamps[i].x;
			params.stamps[i][1] = stamps[i].y;
			params.stamps[i][2] = stamps[i].radius;
			params.stamps[i][3] = stamps[i].depth;

			params.stampParams[i][0] = stamps[i].shoulder;
			params.stampParams[i][1] = stamps[i].decay;

			params.stampParams[i][2] = static_cast<float>(stamps[i].kind);
			params.stampParams[i][3] = stamps[i].rim;

			params.stampShape[i][0] = stamps[i].forwardX;
			params.stampShape[i][1] = stamps[i].forwardY;
			params.stampShape[i][2] = stamps[i].halfWidth;
			params.stampShape[i][3] = stamps[i].mirror;

			params.stampMotion[i][0] = stamps[i].motionX;
			params.stampMotion[i][1] = stamps[i].motionY;
			params.stampMotion[i][2] = stamps[i].snow ? 1.0f : 0.0f;
		}
		FillStampBounds(params, count);

		auto* context = globals::d3d::context;

		{

			const auto fadeFor = [&](uint32_t a_level, float a_out[4]) {
				const float cell = CellSizeFor(a_level);
				const float validHalf = params.control[2] * cell;

				a_out[0] = std::floor(position.x / cell) * cell;
				a_out[1] = std::floor(position.y / cell) * cell;
				a_out[2] = validHalf * 0.80f;
				a_out[3] = validHalf * 0.97f;
			};

			WindowCB window{};
			fadeFor(0, window.centreAndFade);
			if (levels > 1) {
				fadeFor(1, window.window1);
			}

			window.raise[0] = Weather::RaiseScale();
			window.screen[0] = Tessellation::ScreenScale();

			{
				const bool floored = Settings::snowGroundFloor && Settings::enableSnowRaise &&
					Settings::useClipmap && Settings::snowRaiseHeight > 0.0f &&
					SnowCoverage::Ready();

				params.raise[0] = floored ? Settings::snowRaiseHeight : 0.0f;
				params.raise[1] = window.raise[0];
				params.raise[2] = std::max(Settings::snowGroundBite, 0.0f);

				params.raise[3] =
					(floored && Settings::shelterMeshCap && Shelter::View()) ? 1.0f : 0.0f;

				params.raiseWindow[0] = window.centreAndFade[0];
				params.raiseWindow[1] = window.centreAndFade[1];
				params.raiseWindow[3] = Settings::snowRaiseDistance;
				params.raiseWindow[2] = std::max(
					Settings::snowRaiseDistance - Settings::snowRaiseFadeBand, 0.0f);
			}

			g_windowCentreX = window.centreAndFade[0];
			g_windowCentreY = window.centreAndFade[1];

			g_windowHalfExtent = (levels > 1) ? window.window1[3] : window.centreAndFade[3];
			g_windowValid = true;

			D3D11_MAPPED_SUBRESOURCE windowMap{};
			if (SUCCEEDED(context->Map(g_windowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &windowMap))) {
				std::memcpy(windowMap.pData, &window, sizeof(window));
				context->Unmap(g_windowCB, 0);
			}
		}

		{
			const ComputeStageGuard guard(context);

			const UINT noOffset[kComputeUAVs] = { static_cast<UINT>(-1), static_cast<UINT>(-1),
				static_cast<UINT>(-1), static_cast<UINT>(-1) };

			ID3D11ShaderResourceView* shape = StampShapes::View();
			context->CSSetShaderResources(0, 1, &shape);

			ID3D11ShaderResourceView* floorMaps[2] = {
				params.raise[0] > 0.0f ? SnowCoverage::View() : nullptr,
				params.raise[3] > 0.0f ? Shelter::View() : nullptr
			};
			if (!floorMaps[0]) {
				params.raise[0] = 0.0f;
			}
			if (!floorMaps[1]) {
				params.raise[3] = 0.0f;
			}
			context->CSSetShaderResources(3, 2, floorMaps);

			context->CSSetSamplers(0, 1, &g_sampler);

			Profiler::GpuBegin(Profiler::Scope::kClipmap);

			for (uint32_t i = 0; i < levels; ++i) {
				const uint32_t level = levels - 1 - i;
				if (!g_uav[level] || !g_decayUAV[level] || !g_liveSRV[level][1]) {
					continue;
				}

				const float cell = CellSizeFor(level);

				params.window[0] = std::floor(position.x / cell);
				params.window[1] = std::floor(position.y / cell);
				params.window[2] = cell;

				params.activityShape[0] = (kTexels / ActivityTexelsFor(level)) / 8;
				params.activityShape[1] = ActivityTexelsFor(level) - 1;

				const bool  hasCoarser = (level + 1) < levels && g_srv[level + 1];
				params.coarse[0] = hasCoarser ? 1.0f : 0.0f;
				params.coarse[1] = WorldSizeFor(level + 1);
				params.coarse[2] = std::max(Settings::clipmapSeedSmoothing, 0.0f);

				const int32_t nowX = static_cast<int32_t>(params.window[0]);
				const int32_t nowY = static_cast<int32_t>(params.window[1]);

				int32_t moved = static_cast<int32_t>(kTexels);
				if (g_prevWindowValid[level]) {
					moved = std::max(std::abs(nowX - g_prevWindowX[level]),
						std::abs(nowY - g_prevWindowY[level]));
				}
				g_prevWindowX[level] = nowX;
				g_prevWindowY[level] = nowY;
				g_prevWindowValid[level] = true;

				params.coarse[3] = std::clamp(params.control[2] - static_cast<float>(moved) - 2.0f,
					0.0f, params.control[2]);

				// Only the groups that can change run, from a list built on the GPU
				// (kGroupListShader). Every group runs while the flags are not known yet
				// and when the whole window reseeds (a jump, or the first update after
				// indoors), which would list every group anyway.
				const uint32_t parity = g_liveParity[level];
				const bool     listed = Settings::clipmapSkipIdleGroups && g_liveKnown[level] &&
					params.coarse[3] > 0.0f;

				ID3D11ShaderResourceView* seeds[2] = {
					hasCoarser ? g_srv[level + 1] : nullptr,
					hasCoarser ? g_decaySRV[level + 1] : nullptr
				};
				context->CSSetShaderResources(1, 2, seeds);

				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(g_paramsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					break;
				}
				std::memcpy(mapped.pData, &params, sizeof(params));
				context->Unmap(g_paramsCB, 0);

				const UINT zero[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(g_activityUAV[level], zero);

				context->CSSetConstantBuffers(0, 1, &g_paramsCB);

				if (listed) {
					const UINT args[4] = { 64, 0, 1, 0 };
					context->UpdateSubresource(g_groupArgs, 0, nullptr, args, 0, 0);
					context->UpdateSubresource(g_groupCount, 0, nullptr, zero, 0, 0);

					ID3D11UnorderedAccessView* listUAVs[kComputeUAVs] = { g_liveUAV[level][parity ^ 1],
						g_groupArgsUAV, g_groupCountUAV, g_groupListUAV };
					context->CSSetShader(g_groupListCS, nullptr, 0);
					context->CSSetShaderResources(5, 1, &g_liveSRV[level][parity]);
					context->CSSetUnorderedAccessViews(0, kComputeUAVs, listUAVs, noOffset);
					context->Dispatch(kGroups / 8, kGroups / 8, 1);

					ID3D11UnorderedAccessView* nullUAVs[kComputeUAVs] = {};
					context->CSSetUnorderedAccessViews(0, kComputeUAVs, nullUAVs, noOffset);

					ID3D11ShaderResourceView* list[2] = { g_groupListSRV, g_groupCountSRV };
					context->CSSetShaderResources(5, 2, list);
				}

				ID3D11UnorderedAccessView* uavs[kComputeUAVs] = { g_uav[level], g_decayUAV[level],
					g_activityUAV[level], g_liveUAV[level][parity ^ 1] };
				context->CSSetShader(listed ? g_updateListedCS : g_updateCS, nullptr, 0);
				context->CSSetUnorderedAccessViews(0, kComputeUAVs, uavs, noOffset);
				if (listed) {
					context->DispatchIndirect(g_groupArgs, 0);
				} else {
					context->Dispatch(kGroups, kGroups, 1);
				}
				g_liveParity[level] = parity ^ 1;
				g_liveKnown[level] = true;

				ID3D11UnorderedAccessView* nullUAVs[kComputeUAVs] = {};
				context->CSSetUnorderedAccessViews(0, kComputeUAVs, nullUAVs, noOffset);

				ID3D11ShaderResourceView* nullPair[2] = { nullptr, nullptr };
				context->CSSetShaderResources(1, 2, nullPair);
				context->CSSetShaderResources(5, 2, nullPair);
			}

			ID3D11ShaderResourceView* nullFloor[2] = { nullptr, nullptr };
			context->CSSetShaderResources(3, 2, nullFloor);

			Profiler::GpuEnd();
		}
	}

	// Called while Update is skipped (indoors): the next Update reseeds every level as a
	// window jump does, and actor motion starts fresh instead of from before the skip.
	void ForgetWindow()
	{
		for (auto& valid : g_prevWindowValid) {
			valid = false;
		}
		g_actorMotion.clear();
	}

	bool GetWindow(float& a_centreX, float& a_centreY, float& a_halfExtent)
	{
		if (!g_windowValid) {
			return false;
		}
		a_centreX = g_windowCentreX;
		a_centreY = g_windowCentreY;
		a_halfExtent = g_windowHalfExtent;
		return true;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!Ready()) {
			return;
		}
		a_context->DSSetShaderResources(0, 1, &g_srv[0]);
		a_context->DSSetSamplers(0, 1, &g_sampler);
		a_context->DSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);
		a_context->HSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);

		if (g_activitySRV[0]) {
			a_context->HSSetShaderResources(kActivitySlot, 1, &g_activitySRV[0]);
		}

		if (LevelCount() > 1 && g_srv[1]) {
			a_context->DSSetShaderResources(kLevel1Slot, 1, &g_srv[1]);
		}
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{

		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11SamplerState*       nullSampler = nullptr;
		ID3D11Buffer* nullCB = nullptr;
		a_context->DSSetShaderResources(0, 1, &nullSRV);
		a_context->DSSetShaderResources(kLevel1Slot, 1, &nullSRV);
		a_context->HSSetShaderResources(kActivitySlot, 1, &nullSRV);
		a_context->DSSetSamplers(0, 1, &nullSampler);
		a_context->DSSetConstantBuffers(kParamsSlot, 1, &nullCB);
		a_context->HSSetConstantBuffers(kParamsSlot, 1, &nullCB);
	}
}
