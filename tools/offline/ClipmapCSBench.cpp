// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN) and contributors.

// Runs the clipmap update shader from before per-group stamp culling and the current
// one side by side on the GPU. Both get the same resources and the same seeded frames
// (every stamp kind, window moves and jumps, both levels with coarse seeding, dt and
// weather changes), and after every frame Field, DecayRate and Activity must match bit
// for bit. The current shader runs as Clipmap::Update does, through the group lists
// (ClipmapSkipIdleGroups), whose flags must also match its field. Walks over sparse
// marks exercise the lists: idle frames, trails, jumps, reseeds and marks fading back
// to zero, and deliberately broken lists must be caught. With repose on, the shipped
// shaders read neighbours that other groups may already have written this frame, so the
// bit-exact runs with repose on read them from a pre-dispatch copy, and the shipped
// shaders with repose on are only measured (the INFO lines). Then both are timed with
// timestamp queries for 0 to 64 stamps on fresh ground, and with every group, and with
// the lists, over marked ground.
//
//   ClipmapCSBench [--frames N] [--seed S] [--warp] [--no-timing] [--asm DIR]

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ClipmapUpdateCS.h"
#include "ClipmapUpdateCSBaseline.h"

namespace
{
	using Microsoft::WRL::ComPtr;

	constexpr UINT kTexels = Clipmap::kTexels;
	constexpr UINT kLevels = Clipmap::kMaxLevels;
	constexpr UINT kStamps = Clipmap::kMaxStamps;
	constexpr UINT kActivity = Clipmap::kActivityTexels;
	constexpr UINT kGroups = Clipmap::kGroups;

	// Same layout as ParamsCB in Clipmap.cpp.
	struct Params
	{
		float window[4]{};
		float control[4]{};
		float weather[4]{};
		float stamps[kStamps][4]{};
		float stampParams[kStamps][4]{};
		float stampShape[kStamps][4]{};
		float stampMotion[kStamps][4]{};
		float coarse[4]{};
		float rimShape[4]{};
		float snowRim[4]{};
		float raise[4]{};
		float raiseWindow[4]{};
		float stampBounds[kStamps][4]{};  // read only by the current shader
		uint32_t noNoiseMask[4]{};        // read only by the current shader
	};
	static_assert(sizeof(Params) % 16 == 0);

	void Require(bool a_ok, const std::string& a_message)
	{
		if (!a_ok) {
			throw std::runtime_error(a_message);
		}
	}

	void Check(HRESULT a_hr, const char* a_what)
	{
		if (FAILED(a_hr)) {
			char code[32];
			std::snprintf(code, sizeof(code), " (hr=0x%08X)", static_cast<unsigned>(a_hr));
			throw std::runtime_error(std::string(a_what) + code);
		}
	}

	bool SkyrimRunning()
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return false;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		bool found = false;
		for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found;
			 ok = Process32NextW(snapshot, &entry)) {
			found = _wcsicmp(entry.szExeFile, L"SkyrimSE.exe") == 0;
		}
		CloseHandle(snapshot);
		return found;
	}

	std::string CurrentSource()
	{
		return Clipmap::UpdateShaderSource();
	}

	// The same generated constants in front of the ec04e21 shader body.
	std::string BaselineSource()
	{
		const std::string current = Clipmap::UpdateShaderSource();
		const size_t      body = std::strlen(Clipmap::kUpdateShader);
		Require(current.size() > body &&
					current.compare(current.size() - body, body, Clipmap::kUpdateShader) == 0,
			"UpdateShaderSource() no longer ends with kUpdateShader");
		return current.substr(0, current.size() - body) + ClipmapBaseline::kUpdateShader;
	}

	// Repose reads the four neighbours from the texture the pass is writing, so what a
	// texel sees depends on thread timing, in the original shader as much as in the new
	// one. For the exact comparison with repose on, both shaders read the neighbours
	// from a copy taken before the dispatch. The edit is textual and identical for both.
	std::string WithoutNeighbourRace(std::string a_source)
	{
		const std::string from = "= Field[uint2((int2(id.xy) + int2(";
		const std::string to = "= FieldBefore[uint2((int2(id.xy) + int2(";
		int               replaced = 0;
		for (size_t at = a_source.find(from); at != std::string::npos;
			 at = a_source.find(from, at + to.size())) {
			a_source.replace(at, from.size(), to);
			++replaced;
		}
		Require(replaced == 4, "expected the four repose neighbour reads");

		const std::string field = "RWTexture2D<float> Field : register(u0);";
		const size_t      at = a_source.find(field);
		Require(at != std::string::npos, "Field declaration not found");
		a_source.insert(at + field.size(), "\nTexture2D<float> FieldBefore : register(t7);");
		return a_source;
	}

	// Replaces the one occurrence of a_from, for the broken variants the checks must catch.
	std::string ReplaceOnce(std::string a_source, const std::string& a_from, const std::string& a_to)
	{
		const size_t at = a_source.find(a_from);
		Require(at != std::string::npos && a_source.find(a_from, at + 1) == std::string::npos,
			"expected one occurrence of: " + a_from);
		return a_source.replace(at, a_from.size(), a_to);
	}

	// Same defines and flags as CompileComputeShader in Clipmap.cpp.
	ComPtr<ID3DBlob> Compile(const std::string& a_source, const char* a_label, bool a_groupList = false)
	{
		const std::string      maxStamps = std::to_string(kStamps);
		const D3D_SHADER_MACRO defines[] = {
			{ "MAX_STAMPS", maxStamps.c_str() },
			{ a_groupList ? "GROUP_LIST" : nullptr, "1" },  // a null name ends the list
			{ nullptr, nullptr }
		};
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		const HRESULT    hr = D3DCompile(a_source.c_str(), a_source.size(), "ClipmapUpdateCS", defines,
			   nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr)) {
			std::fprintf(stderr, "%s:\n%s\n", a_label,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
		}
		Check(hr, a_label);
		return code;
	}

	void WriteDisassembly(ID3DBlob* a_code, const std::string& a_path)
	{
		ComPtr<ID3DBlob> text;
		Check(D3DDisassemble(a_code->GetBufferPointer(), a_code->GetBufferSize(), 0, nullptr, &text),
			"D3DDisassemble");
		std::ofstream(a_path, std::ios::binary)
			.write(static_cast<const char*>(text->GetBufferPointer()),
				static_cast<std::streamsize>(text->GetBufferSize() - 1));
		std::printf("  wrote %s\n", a_path.c_str());
	}

	struct Level
	{
		ComPtr<ID3D11Texture2D>           field;
		ComPtr<ID3D11UnorderedAccessView> fieldUAV;
		ComPtr<ID3D11ShaderResourceView>  fieldSRV;
		ComPtr<ID3D11Texture2D>           decay;
		ComPtr<ID3D11UnorderedAccessView> decayUAV;
		ComPtr<ID3D11ShaderResourceView>  decaySRV;
		ComPtr<ID3D11Texture2D>           activity;
		ComPtr<ID3D11UnorderedAccessView> activityUAV;
		ComPtr<ID3D11Texture2D>           live[2];
		ComPtr<ID3D11UnorderedAccessView> liveUAV[2];
		ComPtr<ID3D11ShaderResourceView>  liveSRV[2];
		uint32_t                          parity{ 0 };
		bool                              liveKnown{ false };
	};

	// One shader with its own copy of both clipmap levels. The current shader also has
	// its GROUP_LIST variant and the group list pass, and lists groups when skipIdle is set.
	struct Side
	{
		ComPtr<ID3D11ComputeShader>      shader;
		ComPtr<ID3D11ComputeShader>      listedShader;
		ComPtr<ID3D11ComputeShader>      groupList;
		bool                             skipIdle{ false };
		Level                            level[kLevels];
		ComPtr<ID3D11Texture2D>          before;
		ComPtr<ID3D11ShaderResourceView> beforeSRV;
	};

	// How many groups the lists held, per level, over the frames that used them.
	struct ListStats
	{
		int    listedFrames[kLevels]{};
		int    fullFrames[kLevels]{};
		double share[kLevels]{};
		double minShare[kLevels]{ 1.0, 1.0 };
	};

	class Gpu
	{
	public:
		explicit Gpu(bool a_warp)
		{
			const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
			Check(D3D11CreateDevice(nullptr, a_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
					  nullptr, 0, &wanted, 1, D3D11_SDK_VERSION, &_device, nullptr, &_context),
				"D3D11CreateDevice");

			ComPtr<IDXGIDevice>  dxgi;
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC    desc{};
			if (SUCCEEDED(_device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
				SUCCEEDED(adapter->GetDesc(&desc))) {
				std::printf("Adapter: %ls\n", desc.Description);
			}

			std::mt19937 rng(0x5EED0001u);
			_shape = MakeTexture(256, DXGI_FORMAT_R8G8B8A8_UNORM, 4, rng);
			_coverage = MakeTexture(SnowCoverage::kTexels, DXGI_FORMAT_R8_UNORM, 1, rng);
			_meshCap = MakeTexture(Shelter::kTexels, DXGI_FORMAT_R8_UNORM, 1, rng);

			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			Check(_device->CreateSamplerState(&samplerDesc, &_sampler), "CreateSamplerState");

			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(Params);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			Check(_device->CreateBuffer(&cbDesc, nullptr, &_params), "CreateBuffer");

			_readField = Staging(kTexels, DXGI_FORMAT_R32_FLOAT);
			_readDecay = Staging(kTexels, DXGI_FORMAT_R8G8_UNORM);
			_readActivity = Staging(kActivity, DXGI_FORMAT_R32_UINT);
			_readLive = Staging(kGroups, DXGI_FORMAT_R32_UINT);

			// The group list buffers, shared by the levels, as CreateGroupList in Clipmap.cpp.
			D3D11_BUFFER_DESC buffer{};
			buffer.ByteWidth = kGroups * kGroups * 4;
			buffer.Usage = D3D11_USAGE_DEFAULT;
			buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			buffer.StructureByteStride = 4;
			Check(_device->CreateBuffer(&buffer, nullptr, &_list), "list");
			Check(_device->CreateUnorderedAccessView(_list.Get(), nullptr, &_listUAV), "list UAV");
			Check(_device->CreateShaderResourceView(_list.Get(), nullptr, &_listSRV), "list SRV");

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
			buffer.ByteWidth = 16;
			buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			buffer.StructureByteStride = 0;
			Check(_device->CreateBuffer(&buffer, nullptr, &_count), "count");
			Check(_device->CreateUnorderedAccessView(_count.Get(), &rawUAV, &_countUAV), "count UAV");
			Check(_device->CreateShaderResourceView(_count.Get(), &rawSRV, &_countSRV), "count SRV");
			buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			Check(_device->CreateBuffer(&buffer, nullptr, &_args), "args");
			Check(_device->CreateUnorderedAccessView(_args.Get(), &rawUAV, &_argsUAV), "args UAV");
			buffer.Usage = D3D11_USAGE_STAGING;
			buffer.BindFlags = 0;
			buffer.MiscFlags = 0;
			buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (auto& read : _readCount) {
				Check(_device->CreateBuffer(&buffer, nullptr, &read), "count readback");
			}

			D3D11_QUERY_DESC query{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			Check(_device->CreateQuery(&query, &_disjoint), "CreateQuery");
			query.Query = D3D11_QUERY_TIMESTAMP;
			Check(_device->CreateQuery(&query, &_start), "CreateQuery");
			Check(_device->CreateQuery(&query, &_end), "CreateQuery");
		}

		// a_listed and a_groupList: the current shader's GROUP_LIST variant and group list
		// pass; the old shader has neither.
		void CreateSide(Side& a_side, ID3DBlob* a_code, ID3DBlob* a_listed = nullptr, ID3DBlob* a_groupList = nullptr,
			bool a_skipIdle = false)
		{
			const auto create = [&](ID3DBlob* a_blob, ComPtr<ID3D11ComputeShader>& a_out) {
				if (a_blob) {
					Check(_device->CreateComputeShader(a_blob->GetBufferPointer(), a_blob->GetBufferSize(), nullptr,
							  &a_out),
						"CreateComputeShader");
				}
			};
			create(a_code, a_side.shader);
			create(a_listed, a_side.listedShader);
			create(a_groupList, a_side.groupList);
			a_side.skipIdle = a_skipIdle;

			// Formats and bind flags as in CreateLevel in Clipmap.cpp.
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTexels;
			desc.Height = kTexels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			for (auto& level : a_side.level) {
				desc.Width = desc.Height = kTexels;
				desc.Format = DXGI_FORMAT_R32_FLOAT;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.field), "field");
				Check(_device->CreateUnorderedAccessView(level.field.Get(), nullptr, &level.fieldUAV), "field UAV");
				Check(_device->CreateShaderResourceView(level.field.Get(), nullptr, &level.fieldSRV), "field SRV");
				desc.Format = DXGI_FORMAT_R8G8_UNORM;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.decay), "decay");
				Check(_device->CreateUnorderedAccessView(level.decay.Get(), nullptr, &level.decayUAV), "decay UAV");
				Check(_device->CreateShaderResourceView(level.decay.Get(), nullptr, &level.decaySRV), "decay SRV");
				desc.Width = desc.Height = kActivity;
				desc.Format = DXGI_FORMAT_R32_UINT;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.activity), "activity");
				Check(_device->CreateUnorderedAccessView(level.activity.Get(), nullptr, &level.activityUAV),
					"activity UAV");
				desc.Width = desc.Height = kGroups;
				for (int i = 0; i < 2; ++i) {
					Check(_device->CreateTexture2D(&desc, nullptr, &level.live[i]), "live");
					Check(_device->CreateUnorderedAccessView(level.live[i].Get(), nullptr, &level.liveUAV[i]), "live UAV");
					Check(_device->CreateShaderResourceView(level.live[i].Get(), nullptr, &level.liveSRV[i]), "live SRV");
				}
			}
			desc.Width = desc.Height = kTexels;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			Check(_device->CreateTexture2D(&desc, nullptr, &a_side.before), "before");
			Check(_device->CreateShaderResourceView(a_side.before.Get(), nullptr, &a_side.beforeSRV), "before SRV");
		}

		void Load(Side& a_side, uint32_t a_level, const std::vector<float>& a_field,
			const std::vector<uint8_t>& a_decay)
		{
			auto& level = a_side.level[a_level];
			_context->UpdateSubresource(level.field.Get(), 0, nullptr, a_field.data(), kTexels * 4, 0);
			_context->UpdateSubresource(level.decay.Get(), 0, nullptr, a_decay.data(), kTexels * 2, 0);
			level.liveKnown = false;
		}

		// Bare ground, as CreateLevel starts it.
		void Clear(Side& a_side)
		{
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (auto& level : a_side.level) {
				_context->ClearUnorderedAccessViewFloat(level.fieldUAV.Get(), zero);
				_context->ClearUnorderedAccessViewFloat(level.decayUAV.Get(), zero);
				level.liveKnown = false;
			}
		}

		// The per-level half of Clipmap::Update: level 1 first, then level 0 seeded from it.
		// A side with a group list pass lists the groups to update as Clipmap::Update does.
		// With a_stats, the count of each listed level is kept for ListedShare.
		void Update(Side& a_side, const Params (&a_levels)[kLevels], bool a_withoutRace, bool a_stats = false)
		{
			const UINT noOffset[4] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1),
				static_cast<UINT>(-1) };

			ID3D11ShaderResourceView* shape = _shape.Get();
			_context->CSSetShaderResources(0, 1, &shape);
			ID3D11ShaderResourceView* floorMaps[2] = {
				a_levels[0].raise[0] > 0.0f ? _coverage.Get() : nullptr,
				a_levels[0].raise[3] > 0.0f ? _meshCap.Get() : nullptr
			};
			_context->CSSetShaderResources(3, 2, floorMaps);
			_context->CSSetSamplers(0, 1, _sampler.GetAddressOf());

			for (uint32_t i = 0; i < kLevels; ++i) {
				const uint32_t level = kLevels - 1 - i;
				const bool     hasCoarser = level + 1 < kLevels;
				auto&          target = a_side.level[level];

				const uint32_t parity = target.parity;
				const bool     flags = a_side.groupList != nullptr;
				const bool     listed = flags && a_side.skipIdle && target.liveKnown && a_levels[level].coarse[3] > 0.0f;
				_listed[level] = listed;

				ID3D11ShaderResourceView* seeds[2] = {
					hasCoarser ? a_side.level[level + 1].fieldSRV.Get() : nullptr,
					hasCoarser ? a_side.level[level + 1].decaySRV.Get() : nullptr
				};
				_context->CSSetShaderResources(1, 2, seeds);

				D3D11_MAPPED_SUBRESOURCE mapped{};
				Check(_context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map params");
				std::memcpy(mapped.pData, &a_levels[level], sizeof(Params));
				_context->Unmap(_params.Get(), 0);

				const UINT zero[4] = { 0, 0, 0, 0 };
				_context->ClearUnorderedAccessViewUint(target.activityUAV.Get(), zero);

				if (a_withoutRace) {
					_context->CopyResource(a_side.before.Get(), target.field.Get());
					ID3D11ShaderResourceView* before = a_side.beforeSRV.Get();
					_context->CSSetShaderResources(7, 1, &before);
				}

				_context->CSSetConstantBuffers(0, 1, _params.GetAddressOf());

				if (listed) {
					const UINT args[4] = { 64, 0, 1, 0 };
					_context->UpdateSubresource(_args.Get(), 0, nullptr, args, 0, 0);
					_context->UpdateSubresource(_count.Get(), 0, nullptr, zero, 0, 0);

					ID3D11UnorderedAccessView* listUAVs[4] = { target.liveUAV[parity ^ 1].Get(), _argsUAV.Get(),
						_countUAV.Get(), _listUAV.Get() };
					_context->CSSetShader(a_side.groupList.Get(), nullptr, 0);
					_context->CSSetShaderResources(5, 1, target.liveSRV[parity].GetAddressOf());
					_context->CSSetUnorderedAccessViews(0, 4, listUAVs, noOffset);
					_context->Dispatch(kGroups / 8, kGroups / 8, 1);

					ID3D11UnorderedAccessView* nullUAVs[4] = {};
					_context->CSSetUnorderedAccessViews(0, 4, nullUAVs, noOffset);
					if (a_stats) {
						_context->CopyResource(_readCount[level].Get(), _count.Get());
					}

					ID3D11ShaderResourceView* list[2] = { _listSRV.Get(), _countSRV.Get() };
					_context->CSSetShaderResources(5, 2, list);
				}

				ID3D11UnorderedAccessView* uavs[4] = { target.fieldUAV.Get(), target.decayUAV.Get(),
					target.activityUAV.Get(), flags ? target.liveUAV[parity ^ 1].Get() : nullptr };
				_context->CSSetShader(listed ? a_side.listedShader.Get() : a_side.shader.Get(), nullptr, 0);
				_context->CSSetUnorderedAccessViews(0, 4, uavs, noOffset);
				if (listed) {
					_context->DispatchIndirect(_args.Get(), 0);
				} else {
					_context->Dispatch(kGroups, kGroups, 1);
				}
				if (flags) {
					target.parity = parity ^ 1;
					target.liveKnown = true;
				}

				ID3D11UnorderedAccessView* nullUAVs[4] = {};
				_context->CSSetUnorderedAccessViews(0, 4, nullUAVs, noOffset);
				ID3D11ShaderResourceView* nullSRVs[3] = {};
				_context->CSSetShaderResources(1, 2, nullSRVs);
				_context->CSSetShaderResources(5, 3, nullSRVs);
			}
			ID3D11ShaderResourceView* nullSRVs[5] = {};
			_context->CSSetShaderResources(0, 5, nullSRVs);
		}

		// After an Update with a_stats: the share of the level's groups its list held, or
		// a negative value if the level ran every group.
		double ListedShare(uint32_t a_level)
		{
			if (!_listed[a_level]) {
				return -1.0;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(_context->Map(_readCount[a_level].Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map count");
			const uint32_t count = *static_cast<const uint32_t*>(mapped.pData);
			_context->Unmap(_readCount[a_level].Get(), 0);
			return static_cast<double>(count) / static_cast<double>(kGroups * kGroups);
		}

		// Milliseconds of GPU time for one Update, or a negative value if disjoint.
		double TimedUpdate(Side& a_side, const Params (&a_levels)[kLevels])
		{
			_context->Begin(_disjoint.Get());
			_context->End(_start.Get());
			Update(a_side, a_levels, false);
			_context->End(_end.Get());
			_context->End(_disjoint.Get());

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			while (_context->GetData(_disjoint.Get(), &disjoint, sizeof(disjoint), 0) != S_OK) {
			}
			UINT64 start = 0;
			UINT64 end = 0;
			while (_context->GetData(_start.Get(), &start, sizeof(start), 0) != S_OK) {
			}
			while (_context->GetData(_end.Get(), &end, sizeof(end), 0) != S_OK) {
			}
			if (disjoint.Disjoint) {
				return -1.0;
			}
			return static_cast<double>(end - start) * 1000.0 / static_cast<double>(disjoint.Frequency);
		}

		void Read(ID3D11Texture2D* a_source, std::vector<uint8_t>& a_out)
		{
			D3D11_TEXTURE2D_DESC desc{};
			a_source->GetDesc(&desc);
			ID3D11Texture2D* staging = desc.Format == DXGI_FORMAT_R32_FLOAT ? _readField.Get() :
			                           desc.Format == DXGI_FORMAT_R8G8_UNORM ? _readDecay.Get() :
			                           desc.Width == kActivity               ? _readActivity.Get() :
			                                                                   _readLive.Get();
			const UINT rowBytes = desc.Width * (desc.Format == DXGI_FORMAT_R8G8_UNORM ? 2 : 4);

			_context->CopyResource(staging, a_source);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map readback");
			a_out.resize(static_cast<size_t>(rowBytes) * desc.Height);
			for (UINT y = 0; y < desc.Height; ++y) {
				std::memcpy(a_out.data() + static_cast<size_t>(y) * rowBytes,
					static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
			}
			_context->Unmap(staging, 0);
		}

	private:
		ComPtr<ID3D11ShaderResourceView> MakeTexture(UINT a_size, DXGI_FORMAT a_format, UINT a_bytes,
			std::mt19937& a_rng)
		{
			std::vector<uint8_t> data(static_cast<size_t>(a_size) * a_size * a_bytes);
			for (auto& value : data) {
				value = static_cast<uint8_t>(a_rng() >> 24);
			}
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = a_size;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = a_format;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA initial{ data.data(), a_size * a_bytes, 0 };
			ComPtr<ID3D11Texture2D>          texture;
			ComPtr<ID3D11ShaderResourceView> view;
			Check(_device->CreateTexture2D(&desc, &initial, &texture), "input texture");
			Check(_device->CreateShaderResourceView(texture.Get(), nullptr, &view), "input SRV");
			return view;
		}

		ComPtr<ID3D11Texture2D> Staging(UINT a_size, DXGI_FORMAT a_format)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = a_size;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = a_format;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> texture;
			Check(_device->CreateTexture2D(&desc, nullptr, &texture), "staging");
			return texture;
		}

		ComPtr<ID3D11Device>             _device;
		ComPtr<ID3D11DeviceContext>      _context;
		ComPtr<ID3D11ShaderResourceView> _shape;
		ComPtr<ID3D11ShaderResourceView> _coverage;
		ComPtr<ID3D11ShaderResourceView> _meshCap;
		ComPtr<ID3D11SamplerState>       _sampler;
		ComPtr<ID3D11Buffer>             _params;
		ComPtr<ID3D11Texture2D>          _readField;
		ComPtr<ID3D11Texture2D>          _readDecay;
		ComPtr<ID3D11Texture2D>          _readActivity;
		ComPtr<ID3D11Texture2D>          _readLive;
		ComPtr<ID3D11Buffer>             _list;
		ComPtr<ID3D11UnorderedAccessView> _listUAV;
		ComPtr<ID3D11ShaderResourceView> _listSRV;
		ComPtr<ID3D11Buffer>             _count;
		ComPtr<ID3D11UnorderedAccessView> _countUAV;
		ComPtr<ID3D11ShaderResourceView> _countSRV;
		ComPtr<ID3D11Buffer>             _args;
		ComPtr<ID3D11UnorderedAccessView> _argsUAV;
		ComPtr<ID3D11Buffer>             _readCount[kLevels];
		bool                             _listed[kLevels]{};
		ComPtr<ID3D11Query>              _disjoint;
		ComPtr<ID3D11Query>              _start;
		ComPtr<ID3D11Query>              _end;
	};

	struct Coverage
	{
		uint64_t kinds[4]{};  // press, press with rim, melt, print
		uint64_t snow{ 0 };
		uint64_t moving{ 0 };
		int      emptyFrames{ 0 };
		int      fullFrames{ 0 };
		int      jumps{ 0 };
		int      paused{ 0 };
		int      fillFrames{ 0 };
		int      reposeFrames{ 0 };
		int      raiseFrames{ 0 };
		int      tieFrames{ 0 };
		int      fadeFrames{ 0 };
		int      wipeFrames{ 0 };
	};

	// Seeded frames in the shape Clipmap::Update produces them. A walk keeps the player
	// standing, walking or running for a while, with a few stamps near its feet, rarer
	// jumps, idle phases without stamps, and fading phases (long frames, fast rates, now
	// and then a weather fill that wipes every mark to +-0.0), so groups keep dropping
	// out of the lists and coming back.
	class Scenario
	{
	public:
		Scenario(uint32_t a_seed, bool a_repose, bool a_walk = false) :
			_rng(a_seed), _repose(a_repose), _walk(a_walk)
		{
			_x = Uniform(-150000.0f, 150000.0f);
			_y = Uniform(-150000.0f, 150000.0f);
			RandomSettings();
		}

		float X() const { return _x; }
		float Y() const { return _y; }

		void Next(Params (&a_levels)[kLevels], Coverage& a_coverage)
		{
			Move(a_coverage);
			// New settings every dozen frames, and now and then in between.
			if (++_frame % 12 == 0 || Chance(0.04f)) {
				RandomSettings();
			}

			Params p{};
			p.window[3] = static_cast<float>(kTexels);

			float dt = 1.0f / 60.0f + Uniform(-0.004f, 0.004f);
			if (_walk) {
				dt = _fade ? 0.25f : Chance(0.05f) ? 0.0f : dt;
			} else if (Chance(0.1f)) {
				dt = 0.0f;
			} else if (Chance(0.1f)) {
				dt = Uniform(0.05f, 0.4f);
			} else if (Chance(0.1f)) {
				dt = Uniform(0.0f, 0.002f);
			}
			p.control[0] = std::clamp(dt, 0.0f, 0.25f);
			a_coverage.paused += p.control[0] == 0.0f;

			int count = Chance(0.12f) ? 0 : Chance(0.15f) ? static_cast<int>(kStamps) :
			                                                static_cast<int>(_rng() % (kStamps + 1));
			if (_walk) {
				count = _idle ? 0 : Chance(0.05f) ? static_cast<int>(kStamps) : 1 + static_cast<int>(_rng() % 16);
			}
			p.control[1] = static_cast<float>(count);
			a_coverage.emptyFrames += count == 0;
			a_coverage.fullFrames += count == static_cast<int>(kStamps);

			p.control[2] = static_cast<float>(kTexels / 2 - 2);
			p.control[3] = _rimSpan;
			p.weather[0] = _wipe ? Uniform(0.9f, 1.5f) : _fill;
			a_coverage.wipeFrames += _wipe;
			a_coverage.fadeFrames += _fade;
			p.weather[1] = _slope;
			p.weather[2] = _repose ? _reposeRate : 0.0f;
			p.weather[3] = _rimNoise;
			p.rimShape[0] = _lean;
			p.rimShape[1] = _churn;
			p.rimShape[2] = _snowRepose;
			for (int i = 0; i < 4; ++i) {
				p.snowRim[i] = _snowRim[i];
			}
			a_coverage.fillFrames += _fill > 0.0f;
			a_coverage.reposeFrames += _slope > 0.0f;

			const float cell0 = Clipmap::CellSizeFor(0);
			p.raise[0] = _raiseHeight;
			p.raise[1] = _raiseScale;
			p.raise[2] = _bite;
			p.raise[3] = _meshCap;
			p.raiseWindow[0] = std::floor(_x / cell0) * cell0;
			p.raiseWindow[1] = std::floor(_y / cell0) * cell0;
			p.raiseWindow[3] = _raiseDistance;
			p.raiseWindow[2] = std::max(_raiseDistance - _raiseBand, 0.0f);
			a_coverage.raiseFrames += _raiseHeight > 0.0f;

			for (int i = 0; i < count; ++i) {
				Stamp(p, i, a_coverage);
			}
			// Copies of earlier stamps, so equal candidates meet in the ordered comparisons.
			if (count > 1 && Chance(0.25f)) {
				++a_coverage.tieFrames;
				for (int copies = 1 + static_cast<int>(_rng() % 8); copies > 0; --copies) {
					const int from = static_cast<int>(_rng() % count);
					const int to = static_cast<int>(_rng() % count);
					for (int k = 0; k < 4; ++k) {
						p.stamps[to][k] = p.stamps[from][k];
						p.stampShape[to][k] = p.stampShape[from][k];
						p.stampMotion[to][k] = p.stampMotion[from][k];
						if (Chance(0.5f)) {
							p.stampParams[to][k] = p.stampParams[from][k];
						}
					}
					if (Chance(0.3f)) {
						p.stamps[to][3] = -p.stamps[to][3];
					}
				}
			}
			Clipmap::FillStampBounds(p, static_cast<uint32_t>(count));
			// Slide some stamps so that an edge of their rectangle sits on a cell of either
			// level, often a group corner (cells 7 and 0 of eight), exactly or within a cell
			// and a half: there a rectangle that is too tight drops texels.
			if (count > 0 && Chance(0.3f)) {
				for (int i = 0; i < count; ++i) {
					const int   edge = static_cast<int>(_rng() % 4);
					const float at = p.stampBounds[i][edge];
					if (!Chance(0.5f) || !std::isfinite(at)) {
						continue;
					}
					const float cell = Clipmap::CellSizeFor(Chance(0.5f) ? 0 : 1);
					float       k = std::round(at / cell);
					if (Chance(0.5f)) {
						k = std::floor(k / 8.0f) * 8.0f + (Chance(0.5f) ? 0.0f : 7.0f);
					}
					const float nudge = Chance(0.4f) ? 0.0f : cell * Uniform(-1.5f, 1.5f);
					p.stamps[i][edge & 1] += k * cell + nudge - at;
				}
				Clipmap::FillStampBounds(p, static_cast<uint32_t>(count));
			}

			for (uint32_t i = 0; i < kLevels; ++i) {
				const uint32_t level = kLevels - 1 - i;
				const float    cell = Clipmap::CellSizeFor(level);
				Params&        out = a_levels[level];
				out = p;
				out.window[0] = std::floor(_x / cell);
				out.window[1] = std::floor(_y / cell);
				out.window[2] = cell;

				const bool hasCoarser = level + 1 < kLevels;
				out.coarse[0] = hasCoarser ? 1.0f : 0.0f;
				out.coarse[1] = Clipmap::WorldSizeFor(level + 1);
				out.coarse[2] = _smoothing;

				const auto nowX = static_cast<int32_t>(out.window[0]);
				const auto nowY = static_cast<int32_t>(out.window[1]);
				int32_t    moved = static_cast<int32_t>(kTexels);
				if (_prevValid[level]) {
					moved = std::max(std::abs(nowX - _prevX[level]), std::abs(nowY - _prevY[level]));
				}
				_prevX[level] = nowX;
				_prevY[level] = nowY;
				_prevValid[level] = true;
				out.coarse[3] = std::clamp(out.control[2] - static_cast<float>(moved) - 2.0f, 0.0f, out.control[2]);
			}
		}

	private:
		float Uniform(float a_lo, float a_hi) { return std::uniform_real_distribution<float>(a_lo, a_hi)(_rng); }
		bool  Chance(float a_p) { return Uniform(0.0f, 1.0f) < a_p; }

		void Move(Coverage& a_coverage)
		{
			if (_walk) {
				if (--_phaseLeft <= 0) {
					_phaseLeft = 10 + static_cast<int>(_rng() % 50);
					const float pick = Uniform(0.0f, 1.0f);
					const float speed = pick < 0.35f ? 0.0f : pick < 0.75f ? Uniform(1.0f, 9.0f) : Uniform(10.0f, 30.0f);
					const float heading = Uniform(0.0f, 6.2831853f);
					_vx = speed * std::cos(heading);
					_vy = speed * std::sin(heading);
					_idle = Chance(0.3f);
					_fade = Chance(0.35f);
					_wipe = _fade && Chance(0.5f);
				}
				if (Chance(0.004f)) {
					_x = Uniform(-150000.0f, 150000.0f);
					_y = Uniform(-150000.0f, 150000.0f);
					++a_coverage.jumps;
				} else if (Chance(0.01f)) {
					_x += Uniform(-3000.0f, 3000.0f);
					_y += Uniform(-3000.0f, 3000.0f);
					++a_coverage.jumps;
				} else {
					_x += _vx;
					_y += _vy;
				}
				return;
			}
			if (Chance(0.015f)) {
				const float extent = Chance(0.3f) ? 400000.0f : 150000.0f;
				_x = Uniform(-extent, extent);
				_y = Uniform(-extent, extent);
				++a_coverage.jumps;
			} else if (Chance(0.04f)) {
				_x += Uniform(-3000.0f, 3000.0f);
				_y += Uniform(-3000.0f, 3000.0f);
				++a_coverage.jumps;
			} else if (!Chance(0.2f)) {
				_x += Uniform(-9.0f, 9.0f);
				_y += Uniform(-9.0f, 9.0f);
			}
		}

		void RandomSettings()
		{
			const float pick = Uniform(0.0f, 1.0f);
			_rimSpan = pick < 0.1f ? 0.0f : pick < 0.6f ? 1.0f : pick < 0.95f ? Uniform(0.0f, 3.0f) : 16.0f;
			_fill = Chance(0.35f) ? Uniform(0.001f, 0.3f) : 0.0f;
			_slope = _repose && Chance(0.8f) ? std::tan(Uniform(10.0f, 80.0f) * 0.017453292f) : 0.0f;
			_reposeRate = Chance(0.5f) ? 0.01f : Uniform(0.0f, 1.0f);
			_rimNoise = Chance(0.5f) ? 0.0f : Uniform(0.0f, 8.0f);
			_lean = Chance(0.5f) ? 1.0f : Uniform(0.0f, 1.0f);
			_churn = Chance(0.4f) ? 0.45f : Chance(0.3f) ? 0.0f : Uniform(0.0f, 8.0f);
			_snowRepose = Chance(0.5f) ? _reposeRate : Uniform(0.0f, 1.0f);
			_snowRim[0] = Chance(0.3f) ? _rimSpan : Uniform(0.0f, 3.0f);
			_snowRim[1] = Chance(0.3f) ? _rimNoise : Uniform(0.0f, 8.0f);
			_snowRim[2] = Chance(0.3f) ? _lean : Uniform(0.0f, 1.0f);
			_snowRim[3] = Chance(0.3f) ? _churn : Uniform(0.0f, 8.0f);
			const bool floored = Chance(0.6f);
			_raiseHeight = floored ? (Chance(0.5f) ? 35.0f : Uniform(1.0f, 60.0f)) : 0.0f;
			_raiseScale = Uniform(0.0f, 1.5f);
			_bite = Chance(0.6f) ? 0.0f : Uniform(0.0f, 8.0f);
			_meshCap = floored && Chance(0.5f) ? 1.0f : 0.0f;
			_raiseDistance = Chance(0.5f) ? 7936.0f : Uniform(200.0f, 3000.0f);
			_raiseBand = Chance(0.5f) ? 4000.0f : Uniform(0.0f, 2000.0f);
			_smoothing = Chance(0.6f) ? 1.0f : Uniform(0.0f, 3.0f);
		}

		void Stamp(Params& a_p, int a_i, Coverage& a_coverage)
		{
			const float kind = Chance(0.45f) ? 0.0f : Chance(0.4f) ? 1.0f : 2.0f;
			const float region = _walk ? (Chance(0.85f) ? 80.0f : Chance(0.67f) ? 576.0f : 3000.0f) :
			                     Chance(0.85f) ? 576.0f : Chance(0.67f) ? 3000.0f : 30000.0f;
			const float pick = Uniform(0.0f, 1.0f);
			const float radius = _walk ? (pick < 0.7f ? Uniform(4.0f, 20.0f) : pick < 0.9f ? Uniform(20.0f, 60.0f) :
			                                                                                 Uniform(60.0f, 256.0f)) :
			                     pick < 0.7f  ? Uniform(4.0f, 60.0f) :
			                     pick < 0.9f  ? Uniform(60.0f, 256.0f) :
			                     pick < 0.97f ? Uniform(0.5f, 4.0f) :
			                                    0.0f;
			const float angle = Uniform(0.0f, 6.2831853f);
			const bool  snow = Chance(0.4f);

			a_p.stamps[a_i][0] = _x + Uniform(-region, region);
			a_p.stamps[a_i][1] = _y + Uniform(-region, region);
			a_p.stamps[a_i][2] = radius;
			a_p.stamps[a_i][3] = kind == 1.0f ? Uniform(0.0f, 1.2f) : (Chance(0.1f) ? 0.0f : Uniform(0.0f, 40.0f));

			a_p.stampParams[a_i][0] = Uniform(0.05f, 0.95f);
			a_p.stampParams[a_i][1] = !_walk ? Uniform(0.5f, 1.0f) : _fade ? Uniform(0.0f, 0.3f) :
			                          Chance(0.5f) ? 1.0f : Uniform(0.3f, 1.0f);
			a_p.stampParams[a_i][2] = kind;
			a_p.stampParams[a_i][3] = kind == 1.0f ? (Chance(0.5f) ? 0.0f : Uniform(-12.0f, 0.0f)) :
			                          Chance(0.4f) ? 0.0f :
			                                         Uniform(0.0f, 10.0f);

			const bool straight = Chance(0.2f);
			a_p.stampShape[a_i][0] = straight ? 0.0f : std::sin(angle);
			a_p.stampShape[a_i][1] = straight ? 1.0f : std::cos(angle);
			a_p.stampShape[a_i][2] = kind == 2.0f ? Uniform(2.0f, 40.0f) : (Chance(0.5f) ? 0.0f : Uniform(2.0f, 60.0f));
			a_p.stampShape[a_i][3] = Chance(0.5f) ? 1.0f : -1.0f;

			// Melts outside the rimless case: a shoulder of 1 or more (the weight no
			// longer ends at the radius), or no forward with a half width (NaN distance).
			if (kind == 1.0f && Chance(0.15f)) {
				const float edge = Uniform(0.0f, 1.0f);
				a_p.stampParams[a_i][0] = edge < 0.3f ? 0.0f : edge < 0.65f ? 1.0f : 1.5f;
			}
			if (kind == 1.0f && Chance(0.05f)) {
				a_p.stampShape[a_i][0] = a_p.stampShape[a_i][1] = 0.0f;
			}

			const float motion = Uniform(0.0f, 1.0f);
			const float reach = motion < 0.6f ? 0.0f : motion < 0.9f ? 30.0f : 200.0f;
			a_p.stampMotion[a_i][0] = Uniform(-reach, reach);
			a_p.stampMotion[a_i][1] = Uniform(-reach, reach);
			a_p.stampMotion[a_i][2] = snow ? 1.0f : 0.0f;

			a_coverage.kinds[kind == 0.0f ? (a_p.stampParams[a_i][3] > 0.0f ? 1 : 0) : kind == 1.0f ? 2 : 3]++;
			a_coverage.snow += snow;
			a_coverage.moving += reach > 0.0f;
		}

		std::mt19937 _rng;
		bool         _repose;
		bool         _walk;
		int          _frame{ 0 };
		float        _x{ 0.0f };
		float        _y{ 0.0f };
		int32_t      _prevX[kLevels]{};
		int32_t      _prevY[kLevels]{};
		bool         _prevValid[kLevels]{};

		int   _phaseLeft{ 0 };
		float _vx{ 0.0f };
		float _vy{ 0.0f };
		bool  _idle{ false };
		bool  _fade{ false };
		bool  _wipe{ false };

		float _rimSpan{ 1.0f };
		float _fill{ 0.0f };
		float _slope{ 0.0f };
		float _reposeRate{ 0.01f };
		float _rimNoise{ 0.0f };
		float _lean{ 1.0f };
		float _churn{ 0.45f };
		float _snowRepose{ 0.1f };
		float _snowRim[4]{};
		float _raiseHeight{ 0.0f };
		float _raiseScale{ 1.0f };
		float _bite{ 0.0f };
		float _meshCap{ 0.0f };
		float _raiseDistance{ 7936.0f };
		float _raiseBand{ 4000.0f };
		float _smoothing{ 1.0f };
	};

	// Old marks and bare ground, identical for both sides.
	void LoadStartingState(Gpu& a_gpu, Side& a_one, Side& a_two, uint32_t a_seed)
	{
		std::mt19937                          rng(a_seed);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		std::vector<float>                    field(static_cast<size_t>(kTexels) * kTexels);
		std::vector<uint8_t>                  decay(field.size() * 2);
		for (uint32_t level = 0; level < kLevels; ++level) {
			for (size_t i = 0; i < field.size(); ++i) {
				const float pick = unit(rng);
				field[i] = pick < 0.45f ? 0.0f : pick < 0.47f ? -0.0f : pick < 0.8f ? -30.0f * unit(rng) :
				           pick < 0.97f                          ? 8.0f * unit(rng) :
				                                                   400.0f * (unit(rng) - 0.5f);
				decay[i * 2] = static_cast<uint8_t>(rng() >> 24);
				decay[i * 2 + 1] = static_cast<uint8_t>(rng() >> 24);
			}
			a_gpu.Load(a_one, level, field, decay);
			a_gpu.Load(a_two, level, field, decay);
		}
	}

	// Walked ground around (a_x, a_y): two rows of footprints (radius 11 u, up to 1.2 u
	// deep, a 0.3 u rim) along a_near random walks that start within +/-600 u and a_far
	// within +/-2800 u, a_length u each. With a_all, the texels off the trails hold -0.5
	// instead of bare ground. A negative a_rate gives each walk a random rate byte and
	// snow flag; otherwise the rate byte is a_rate (255: stored rate 1.0, never fades)
	// and the marks are not snow, so the snow floor leaves them as they are. The snow
	// byte is 0 or 255, as the update writes it.
	struct Marks
	{
		std::vector<float>   field[kLevels];
		std::vector<uint8_t> decay[kLevels];
		double               liveShare[kLevels]{};  // groups holding a non-zero texel
	};

	Marks MakeTrails(float a_x, float a_y, int a_near, int a_far, float a_length, bool a_all, int a_rate,
		uint32_t a_seed)
	{
		Marks                                 out;
		std::mt19937                          rng(a_seed);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		struct Print
		{
			float   x, y;
			uint8_t rate, snow;
		};
		std::vector<Print> prints;
		for (int walk = 0; walk < a_near + a_far; ++walk) {
			const float   spread = walk < a_near ? 600.0f : 2800.0f;
			float         x = a_x + (unit(rng) * 2.0f - 1.0f) * spread;
			float         y = a_y + (unit(rng) * 2.0f - 1.0f) * spread;
			float         heading = unit(rng) * 6.2831853f;
			const uint8_t rate = a_rate >= 0 ? static_cast<uint8_t>(a_rate) : static_cast<uint8_t>(rng() >> 24);
			const uint8_t snow = a_rate < 0 && unit(rng) < 0.5f ? 255 : 0;
			for (float walked = 0.0f; walked < a_length; walked += 22.0f) {
				heading += (unit(rng) - 0.5f) * 0.25f;
				const float side = (static_cast<int>(walked / 22.0f) & 1) ? 9.0f : -9.0f;
				x += std::cos(heading) * 22.0f;
				y += std::sin(heading) * 22.0f;
				prints.push_back({ x - std::sin(heading) * side, y + std::cos(heading) * side, rate, snow });
			}
		}
		for (uint32_t level = 0; level < kLevels; ++level) {
			auto& field = out.field[level];
			auto& decay = out.decay[level];
			field.assign(static_cast<size_t>(kTexels) * kTexels, a_all ? -0.5f : 0.0f);
			decay.assign(field.size() * 2, 0);
			for (size_t i = 0; a_all && i < field.size(); ++i) {
				decay[i * 2] = 255;
			}
			const float cell = Clipmap::CellSizeFor(level);
			const int   cx = static_cast<int>(std::floor(a_x / cell));
			const int   cy = static_cast<int>(std::floor(a_y / cell));
			const int   half = static_cast<int>(kTexels / 2);
			for (const auto& print : prints) {
				const float reach = 13.0f;
				const int   x0 = std::max(static_cast<int>(std::floor((print.x - reach) / cell)), cx - half);
				const int   x1 = std::min(static_cast<int>(std::floor((print.x + reach) / cell)), cx + half - 1);
				const int   y0 = std::max(static_cast<int>(std::floor((print.y - reach) / cell)), cy - half);
				const int   y1 = std::min(static_cast<int>(std::floor((print.y + reach) / cell)), cy + half - 1);
				for (int y = y0; y <= y1; ++y) {
					for (int x = x0; x <= x1; ++x) {
						const float dx = x * cell - print.x;
						const float dy = y * cell - print.y;
						const float d = std::sqrt(dx * dx + dy * dy);
						if (d > reach) {
							continue;
						}
						const float  h = d < 11.0f ? -1.2f * (1.0f - d / 22.0f) : 0.3f;
						const size_t at = static_cast<size_t>(y & (kTexels - 1)) * kTexels +
						                  static_cast<size_t>(x & (kTexels - 1));
						field[at] = h < 0.0f ? std::min(field[at], h) : field[at] < 0.0f ? field[at] : std::max(field[at], h);
						decay[at * 2] = print.rate;
						decay[at * 2 + 1] = print.snow;
					}
				}
			}
			size_t live = 0;
			for (UINT gy = 0; gy < kGroups; ++gy) {
				for (UINT gx = 0; gx < kGroups; ++gx) {
					bool any = false;
					for (UINT y = 0; y < 8 && !any; ++y) {
						for (UINT x = 0; x < 8 && !any; ++x) {
							any = field[(static_cast<size_t>(gy) * 8 + y) * kTexels + gx * 8 + x] != 0.0f;
						}
					}
					live += any;
				}
			}
			out.liveShare[level] = static_cast<double>(live) / static_cast<double>(kGroups * kGroups);
		}
		return out;
	}

	void LoadMarks(Gpu& a_gpu, Side& a_side, const Marks& a_marks)
	{
		for (uint32_t level = 0; level < kLevels; ++level) {
			a_gpu.Load(a_side, level, a_marks.field[level], a_marks.decay[level]);
		}
	}

	struct Difference
	{
		size_t texels{ 0 };
		size_t first{ 0 };
		double maxAbs{ 0.0 };
	};

	// a_stride 4 compares floats (Field) or uints (Activity), 2 compares R8G8 texels.
	Difference Compare(const std::vector<uint8_t>& a_one, const std::vector<uint8_t>& a_two, size_t a_stride,
		bool a_float)
	{
		Difference result;
		if (a_one.size() == a_two.size() && std::memcmp(a_one.data(), a_two.data(), a_one.size()) == 0) {
			return result;
		}
		for (size_t i = 0; i + a_stride <= a_one.size(); i += a_stride) {
			if (std::memcmp(a_one.data() + i, a_two.data() + i, a_stride) == 0) {
				continue;
			}
			if (result.texels++ == 0) {
				result.first = i / a_stride;
			}
			double one = 0.0;
			double two = 0.0;
			if (a_stride == 4) {
				float    f[2];
				uint32_t u[2];
				std::memcpy(&f[0], a_one.data() + i, 4);
				std::memcpy(&f[1], a_two.data() + i, 4);
				std::memcpy(&u[0], a_one.data() + i, 4);
				std::memcpy(&u[1], a_two.data() + i, 4);
				one = a_float ? f[0] : u[0];
				two = a_float ? f[1] : u[1];
			} else {
				one = a_one[i] + a_one[i + 1] * 256.0;
				two = a_two[i] + a_two[i + 1] * 256.0;
			}
			result.maxAbs = std::max(result.maxAbs, std::abs(one - two));
			if (std::isnan(one) != std::isnan(two)) {
				result.maxAbs = INFINITY;
			}
		}
		return result;
	}

	// Differing texels between the two sides' clipmaps, per texture, summed over levels.
	struct FrameDifference
	{
		Difference field, decay, activity;
		uint32_t   level{ 0 };
		bool       Any() const { return field.texels || decay.texels || activity.texels; }
	};

	FrameDifference CompareSides(Gpu& a_gpu, Side& a_one, Side& a_two)
	{
		static std::vector<uint8_t> one;
		static std::vector<uint8_t> two;
		FrameDifference             total;
		for (uint32_t level = 0; level < kLevels; ++level) {
			const auto add = [&](Difference& a_total, ID3D11Texture2D* a_a, ID3D11Texture2D* a_b, size_t a_stride,
								 bool a_float) {
				a_gpu.Read(a_a, one);
				a_gpu.Read(a_b, two);
				const auto d = Compare(one, two, a_stride, a_float);
				if (d.texels && !total.Any()) {
					total.level = level;
				}
				if (d.texels && !a_total.texels) {
					a_total.first = d.first;
				}
				a_total.texels += d.texels;
				a_total.maxAbs = std::max(a_total.maxAbs, d.maxAbs);
			};
			add(total.field, a_one.level[level].field.Get(), a_two.level[level].field.Get(), 4, true);
			add(total.decay, a_one.level[level].decay.Get(), a_two.level[level].decay.Get(), 2, false);
			add(total.activity, a_one.level[level].activity.Get(), a_two.level[level].activity.Get(), 4, false);
		}
		return total;
	}

	// The flags a side with group lists wrote last must be 1 exactly for the groups that
	// hold a texel that is not +0.0. Returns how many groups disagree, over both levels.
	size_t WrongFlags(Gpu& a_gpu, Side& a_side)
	{
		static std::vector<uint8_t> field;
		static std::vector<uint8_t> live;
		size_t                      wrong = 0;
		for (auto& level : a_side.level) {
			a_gpu.Read(level.field.Get(), field);
			a_gpu.Read(level.live[level.parity].Get(), live);
			for (UINT gy = 0; gy < kGroups; ++gy) {
				for (UINT gx = 0; gx < kGroups; ++gx) {
					bool any = false;
					for (UINT y = 0; y < 8 && !any; ++y) {
						const uint8_t* row = field.data() + ((static_cast<size_t>(gy) * 8 + y) * kTexels + gx * 8) * 4;
						for (UINT byte = 0; byte < 32 && !any; ++byte) {
							any = row[byte] != 0;
						}
					}
					uint32_t flag = 0;
					std::memcpy(&flag, live.data() + (static_cast<size_t>(gy) * kGroups + gx) * 4, 4);
					wrong += any != (flag != 0);
				}
			}
		}
		return wrong;
	}

	std::string Describe(const FrameDifference& a_d)
	{
		char text[256];
		std::snprintf(text, sizeof(text),
			"level %u: Field %zu texels (max |diff| %g, first at %zu,%zu), DecayRate %zu, Activity %zu",
			a_d.level, a_d.field.texels, a_d.field.maxAbs, a_d.field.first % kTexels, a_d.field.first / kTexels,
			a_d.decay.texels, a_d.activity.texels);
		return text;
	}

	// Runs both sides through the same frames. With a_exact, every frame must match. A
	// walk starts from sparse trails instead of old marks everywhere. With a_broken,
	// a_two is a deliberately broken variant that must differ within the frames. The
	// flags of a side with group lists must match its field after every frame. Returns
	// the number of frames that differed.
	int RunFrames(Gpu& a_gpu, Side& a_one, Side& a_two, const char* a_name, int a_frames, uint32_t a_seed,
		bool a_repose, bool a_withoutRace, bool a_exact, bool a_walk = false, bool a_broken = false)
	{
		Scenario scenario(a_seed, a_repose, a_walk);
		if (a_walk) {
			const Marks marks = MakeTrails(scenario.X(), scenario.Y(), 4, 6, 1500.0f, false, -1, a_seed * 7919u + 1u);
			LoadMarks(a_gpu, a_one, marks);
			LoadMarks(a_gpu, a_two, marks);
		} else {
			LoadStartingState(a_gpu, a_one, a_two, a_seed * 7919u + 1u);
		}
		Coverage  coverage;
		ListStats lists;
		int       differing = 0;
		size_t    worstTexels = 0;
		size_t    fieldTexels = 0;
		double    fieldMaxAbs = 0.0;
		for (int frame = 0; frame < a_frames; ++frame) {
			Params levels[kLevels];
			scenario.Next(levels, coverage);
			a_gpu.Update(a_one, levels, a_withoutRace);
			a_gpu.Update(a_two, levels, a_withoutRace, true);
			if (a_two.groupList) {
				for (uint32_t level = 0; level < kLevels; ++level) {
					const double share = a_gpu.ListedShare(level);
					if (share < 0.0) {
						++lists.fullFrames[level];
						continue;
					}
					++lists.listedFrames[level];
					lists.share[level] += share;
					lists.minShare[level] = std::min(lists.minShare[level], share);
				}
				const size_t wrong = a_broken ? 0 : WrongFlags(a_gpu, a_two);
				Require(wrong == 0, std::string(a_name) + ": frame " + std::to_string(frame) + ", " +
										std::to_string(wrong) + " group flags do not match the field");
			}
			const auto d = CompareSides(a_gpu, a_one, a_two);
			if (d.Any()) {
				++differing;
				worstTexels = std::max(worstTexels, d.field.texels + d.decay.texels + d.activity.texels);
				fieldTexels += d.field.texels;
				fieldMaxAbs = std::max(fieldMaxAbs, d.field.maxAbs);
				if (a_broken) {
					std::printf("PASS %s: caught at frame %d, %s\n", a_name, frame, Describe(d).c_str());
					return differing;
				}
				if (a_exact) {
					throw std::runtime_error(std::string(a_name) + ": frame " + std::to_string(frame) +
											 " differs, " + Describe(d));
				}
			}
		}
		Require(!a_broken, std::string(a_name) + ": the broken variant matched for " + std::to_string(a_frames) +
							   " frames, so the checks cannot tell");
		std::printf("%s %s: %d frames x %u levels, %d differing (worst %zu texels)\n",
			a_exact ? (differing ? "FAIL" : "PASS") : "INFO", a_name, a_frames, kLevels, differing, worstTexels);
		if (!a_exact) {
			std::printf("       Field: %zu texels differ over all frames, max |diff| %g\n", fieldTexels, fieldMaxAbs);
		}
		std::printf("       stamps: %llu press, %llu press+rim, %llu melt, %llu print (%llu snow, %llu swept); "
					"frames: %d empty, %d full, %d jumps, %d paused, %d fill, %d repose, %d raise, %d ties, "
					"%d fading, %d wiped\n",
			coverage.kinds[0], coverage.kinds[1], coverage.kinds[2], coverage.kinds[3], coverage.snow,
			coverage.moving, coverage.emptyFrames, coverage.fullFrames, coverage.jumps, coverage.paused,
			coverage.fillFrames, coverage.reposeFrames, coverage.raiseFrames, coverage.tieFrames,
			coverage.fadeFrames, coverage.wipeFrames);
		if (a_two.groupList) {
			std::printf("       group lists:");
			for (uint32_t i = 0; i < kLevels; ++i) {
				const uint32_t level = kLevels - 1 - i;
				const int      listed = lists.listedFrames[level];
				std::printf(" level %u listed on %d frames, every group on %d, %.1f%% of groups on average (min "
							"%.1f%%);",
					level, listed, lists.fullFrames[level], listed ? 100.0 * lists.share[level] / listed : 0.0,
					listed ? 100.0 * lists.minShare[level] : 0.0);
			}
			std::printf(" flags matched the field every frame\n");
		}
		return differing;
	}

	constexpr float kPlayerX = 20000.3f;
	constexpr float kPlayerY = -35000.7f;

	// Timing layouts, the player standing still: small prints and presses within
	// +/-400 u of the player, optionally with heat melts (forty lantern sized, six or
	// four at radius 256, or two torches), or moved out of both windows to show what
	// the culling itself costs.
	void TimingParams(int a_count, int a_mode, Params (&a_levels)[kLevels])
	{
		const float  px = kPlayerX;
		const float  py = kPlayerY;
		std::mt19937 rng(1234u + static_cast<uint32_t>(a_count * 3 + a_mode));
		std::uniform_real_distribution<float> close(-400.0f, 400.0f);
		std::uniform_real_distribution<float> wide(-550.0f, 550.0f);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);

		Params p{};
		p.window[3] = static_cast<float>(kTexels);
		p.control[0] = 1.0f / 60.0f;
		p.control[1] = static_cast<float>(a_count);
		p.control[2] = static_cast<float>(kTexels / 2 - 2);
		p.control[3] = 1.0f;
		p.weather[1] = std::tan(50.0f * 0.017453292f);
		p.weather[2] = 0.01f;
		p.rimShape[0] = 1.0f;
		p.rimShape[1] = 0.45f;
		p.rimShape[2] = 0.10f;
		p.snowRim[0] = 0.7f;
		p.snowRim[1] = 0.8f;
		p.snowRim[2] = 1.0f;
		p.snowRim[3] = 0.8f;
		p.raise[0] = 35.0f;
		p.raise[1] = 1.0f;
		p.raise[3] = 1.0f;
		p.raiseWindow[0] = std::floor(px / Clipmap::CellSizeFor(0)) * Clipmap::CellSizeFor(0);
		p.raiseWindow[1] = std::floor(py / Clipmap::CellSizeFor(0)) * Clipmap::CellSizeFor(0);
		p.raiseWindow[2] = 3936.0f;
		p.raiseWindow[3] = 7936.0f;
		for (int i = 0; i < a_count; ++i) {
			float r = 8.0f + 6.0f * unit(rng);
			float kind = i % 3 == 0 ? 2.0f : 0.0f;
			float x = px + close(rng);
			float y = py + close(rng);
			float halfWidth = kind > 1.5f ? r * 0.5f : 0.0f;
			float rim = 1.2f * 1.45f * 0.2f;
			float depth = 6.0f * 0.2f;
			if ((a_mode == 1 || a_mode == 2) && i < 40) {
				r = 20.0f + 40.0f * unit(rng);
				kind = 1.0f;
				x = px + wide(rng);
				y = py + wide(rng);
				halfWidth = 0.0f;
				rim = 0.0f;
				depth = 0.85f;
			}
			if (a_mode == 2 && i < 6) {
				r = 256.0f;
			}
			// 4: the first four are light melts at HeatMaxRadius; 5: the first two are
			// carried torches (HeatTorchRadius 40).
			if ((a_mode == 4 && i < 4) || (a_mode == 5 && i < 2)) {
				r = a_mode == 4 ? 256.0f : 40.0f;
				kind = 1.0f;
				x = px + (a_mode == 4 ? wide(rng) : close(rng));
				y = py + (a_mode == 4 ? wide(rng) : close(rng));
				halfWidth = 0.0f;
				rim = 0.0f;
				depth = 0.85f;
			}
			if (a_mode == 3) {
				x += 5000.0f;
			}
			p.stamps[i][0] = x;
			p.stamps[i][1] = y;
			p.stamps[i][2] = r;
			p.stamps[i][3] = depth;
			p.stampParams[i][0] = 0.45f;
			p.stampParams[i][1] = 1.0f;
			p.stampParams[i][2] = kind;
			p.stampParams[i][3] = rim;
			p.stampShape[i][1] = 1.0f;
			p.stampShape[i][2] = halfWidth;
			p.stampShape[i][3] = 1.0f;
		}
		Clipmap::FillStampBounds(p, static_cast<uint32_t>(a_count));
		for (uint32_t level = 0; level < kLevels; ++level) {
			const float cell = Clipmap::CellSizeFor(level);
			a_levels[level] = p;
			a_levels[level].window[0] = std::floor(px / cell);
			a_levels[level].window[1] = std::floor(py / cell);
			a_levels[level].window[2] = cell;
			a_levels[level].coarse[0] = level + 1 < kLevels ? 1.0f : 0.0f;
			a_levels[level].coarse[1] = Clipmap::WorldSizeFor(level + 1);
			a_levels[level].coarse[2] = 1.0f;
			a_levels[level].coarse[3] = p.control[2] - 2.0f;
		}
	}

	void Timing(Gpu& a_gpu, Side& a_old, Side& a_new)
	{
		struct Case
		{
			int         count;
			int         mode;
			const char* name;
		};
		const Case cases[] = {
			{ 0, 0, "0 stamps" },
			{ 8, 0, "8 small" },
			{ 16, 0, "16 small" },
			{ 32, 0, "32 small" },
			{ 64, 0, "64 small" },
			{ 64, 1, "64 interior mix (40 melts r20-60)" },
			{ 64, 2, "64 incl. six r=256 melts" },
			{ 64, 3, "64 small, 5000 u away (all culled)" },
			{ 20, 4, "16 small + 4 r=256 light melts" },
			{ 12, 5, "10 small + 2 torch melts r=40" },
		};
		constexpr int kRounds = 10;
		constexpr int kWarmup = 10;
		constexpr int kFrames = 30;

		std::vector<double> times[std::size(cases)][2];
		for (int round = 0; round < kRounds; ++round) {
			for (size_t c = 0; c < std::size(cases); ++c) {
				Params levels[kLevels];
				TimingParams(cases[c].count, cases[c].mode, levels);
				for (int which = 0; which < 2; ++which) {
					Side& side = (round + which) % 2 ? a_new : a_old;
					auto& out = times[c][&side == &a_new];
					a_gpu.Clear(side);
					for (int frame = 0; frame < kWarmup + kFrames; ++frame) {
						const double ms = a_gpu.TimedUpdate(side, levels);
						if (frame >= kWarmup && ms >= 0.0) {
							out.push_back(ms);
						}
					}
				}
			}
		}

		std::printf("\nGPU time per frame on fresh ground, both levels (median of %d rounds x %d frames; p10-p90)\n",
			kRounds, kFrames);
		std::printf("  %-36s %22s %22s %8s\n", "case", "ec04e21", "current", "speedup");
		for (size_t c = 0; c < std::size(cases); ++c) {
			double median[2];
			double p10[2];
			double p90[2];
			for (int i = 0; i < 2; ++i) {
				auto& v = times[c][i];
				std::sort(v.begin(), v.end());
				median[i] = v[v.size() / 2];
				p10[i] = v[v.size() / 10];
				p90[i] = v[v.size() * 9 / 10];
			}
			std::printf("  %-36s %6.3f ms (%5.3f-%5.3f) %6.3f ms (%5.3f-%5.3f) %7.2fx\n", cases[c].name, median[0],
				p10[0], p90[0], median[1], p10[1], p90[1], median[0] / median[1]);
		}
	}

	// a_base with the player and its stamps moved by (a_dx, a_dy): the windows follow, and
	// the ring is the one that move opens.
	void Place(const Params (&a_base)[kLevels], float a_dx, float a_dy, int32_t (&a_window)[kLevels][2],
		Params (&a_out)[kLevels])
	{
		const float cell0 = Clipmap::CellSizeFor(0);
		for (uint32_t level = 0; level < kLevels; ++level) {
			Params& p = a_out[level];
			p = a_base[level];
			const auto count = static_cast<uint32_t>(p.control[1]);
			for (uint32_t i = 0; i < count; ++i) {
				p.stamps[i][0] += a_dx;
				p.stamps[i][1] += a_dy;
			}
			Clipmap::FillStampBounds(p, count);
			const float cell = Clipmap::CellSizeFor(level);
			p.window[0] = std::floor((kPlayerX + a_dx) / cell);
			p.window[1] = std::floor((kPlayerY + a_dy) / cell);
			const auto    nowX = static_cast<int32_t>(p.window[0]);
			const auto    nowY = static_cast<int32_t>(p.window[1]);
			const int32_t moved = std::max(std::abs(nowX - a_window[level][0]), std::abs(nowY - a_window[level][1]));
			a_window[level][0] = nowX;
			a_window[level][1] = nowY;
			p.coarse[3] = std::clamp(p.control[2] - static_cast<float>(moved) - 2.0f, 0.0f, p.control[2]);
			p.raiseWindow[0] = std::floor((kPlayerX + a_dx) / cell0) * cell0;
			p.raiseWindow[1] = std::floor((kPlayerY + a_dy) / cell0) * cell0;
		}
	}

	// Marked ground (trails of never-fading prints, as with a stored rate of 1.0), heat
	// melts and a moving window, three ways: the old shader over every group, the current
	// shader over every group (ClipmapSkipIdleGroups = 0), and over the listed groups.
	// Each side updates its own copy of the same ground; frames interleave the three.
	void ScheduleTiming(Gpu& a_gpu, Side& a_old, Side& a_every, Side& a_listed)
	{
		struct Case
		{
			const char* name;
			int         nearWalks;
			int         farWalks;
			float       length;
			bool        all;
			int         stamps;
			int         heat;
			float       heatReach;
			float       speed;  // u per frame, the stamps moving with the player
		};
		const Case cases[] = {
			{ "fresh ground, 0 stamps", 0, 0, 0.0f, false, 0, 0, 0.0f, 0.0f },
			{ "fresh ground, 16 stamps", 0, 0, 0.0f, false, 16, 0, 0.0f, 0.0f },
			{ "light trails, 16 stamps", 3, 4, 1500.0f, false, 16, 0, 0.0f, 0.0f },
			{ "busy trails, 16 stamps", 10, 12, 2500.0f, false, 16, 0, 0.0f, 0.0f },
			{ "heavy trails, 64 stamps", 30, 30, 3000.0f, false, 64, 0, 0.0f, 0.0f },
			{ "walking 6 u/frame, light trails", 3, 4, 1500.0f, false, 12, 0, 0.0f, 6.0f },
			{ "running 18 u/frame, light trails", 3, 4, 1500.0f, false, 8, 0, 0.0f, 18.0f },
			{ "camp: 2 heat r=60, light trails", 3, 4, 1500.0f, false, 12, 2, 60.0f, 0.0f },
			{ "town: 4 heat r=100, busy trails", 10, 12, 2500.0f, false, 16, 4, 100.0f, 0.0f },
			{ "town: 8 heat r=256, busy trails", 10, 12, 2500.0f, false, 16, 8, 256.0f, 0.0f },
			{ "every texel marked, 16 stamps", 0, 0, 0.0f, true, 16, 0, 0.0f, 0.0f },
		};
		constexpr int kWarmup = 10;
		constexpr int kFrames = 240;
		constexpr int kSides = 3;
		Side* const   sides[kSides] = { &a_old, &a_every, &a_listed };

		std::printf("\nGPU time per frame over marked ground, both levels (frames interleaved, %d each; min / median)\n",
			kFrames);
		std::printf("  %-34s %13s %13s %15s %15s %15s\n", "case", "live L0/L1", "listed L0/L1", "ec04e21",
			"every group", "listed");
		for (const auto& c : cases) {
			Params base[kLevels];
			TimingParams(c.stamps, 0, base);
			std::mt19937                          rng(99u + static_cast<uint32_t>(c.heat));
			std::uniform_real_distribution<float> spread(-500.0f, 500.0f);
			for (int h = 0; h < c.heat; ++h) {
				const float x = kPlayerX + spread(rng);
				const float y = kPlayerY + spread(rng);
				for (auto& p : base) {
					const int i = c.stamps + h;
					p.stamps[i][0] = x;
					p.stamps[i][1] = y;
					p.stamps[i][2] = c.heatReach;
					p.stamps[i][3] = 0.85f;
					p.stampParams[i][0] = 0.2f;
					p.stampParams[i][1] = 1.0f;
					p.stampParams[i][2] = 1.0f;
					p.stampParams[i][3] = 0.0f;
					p.stampShape[i][1] = 1.0f;
					p.stampShape[i][3] = 1.0f;
					p.control[1] = static_cast<float>(c.stamps + c.heat);
				}
			}

			const bool  bare = c.nearWalks + c.farWalks == 0 && !c.all;
			const Marks marks = bare ? Marks{} :
			                           MakeTrails(kPlayerX, kPlayerY, c.nearWalks, c.farWalks, c.length, c.all, 255,
										   777u + static_cast<uint32_t>(c.nearWalks * 31 + c.farWalks));
			for (Side* side : sides) {
				if (bare) {
					a_gpu.Clear(*side);
				} else {
					LoadMarks(a_gpu, *side, marks);
				}
			}

			// One untimed frame in place, so the lists start from known flags.
			int32_t window[kLevels][2];
			for (uint32_t level = 0; level < kLevels; ++level) {
				window[level][0] = static_cast<int32_t>(base[level].window[0]);
				window[level][1] = static_cast<int32_t>(base[level].window[1]);
			}
			Params levels[kLevels];
			Place(base, 0.0f, 0.0f, window, levels);
			for (Side* side : sides) {
				a_gpu.Update(*side, levels, false);
			}

			std::vector<double> times[kSides];
			double              listed[kLevels]{};
			for (int frame = 1; frame <= kWarmup + kFrames + 1; ++frame) {
				const float walked = c.speed * static_cast<float>(frame);
				Place(base, walked * 0.8f, walked * 0.6f, window, levels);
				if (frame > kWarmup + kFrames) {
					a_gpu.Update(a_listed, levels, false, true);
					for (uint32_t level = 0; level < kLevels; ++level) {
						listed[level] = a_gpu.ListedShare(level);
					}
					break;
				}
				for (int k = 0; k < kSides; ++k) {
					const int    v = (k + frame) % kSides;
					const double ms = a_gpu.TimedUpdate(*sides[v], levels);
					if (frame > kWarmup && ms >= 0.0) {
						times[v].push_back(ms);
					}
				}
			}

			char cell[kSides][32];
			for (int v = 0; v < kSides; ++v) {
				auto& t = times[v];
				std::sort(t.begin(), t.end());
				if (t.empty()) {
					std::snprintf(cell[v], sizeof(cell[v]), "no samples");
				} else {
					std::snprintf(cell[v], sizeof(cell[v]), "%.3f / %.3f", t.front(), t[t.size() / 2]);
				}
			}
			char live[32];
			char share[32];
			std::snprintf(live, sizeof(live), "%.1f/%.1f%%", 100.0 * marks.liveShare[0], 100.0 * marks.liveShare[1]);
			std::snprintf(share, sizeof(share), "%.1f/%.1f%%", 100.0 * std::max(listed[0], 0.0),
				100.0 * std::max(listed[1], 0.0));
			std::printf("  %-34s %13s %13s %15s %15s %15s\n", c.name, live, share, cell[0], cell[1], cell[2]);
			std::fflush(stdout);
		}
	}
}

int main(int a_argc, char** a_argv)
{
	int         frames = 320;
	uint32_t    seed = 20260923u;
	bool        warp = false;
	bool        timing = true;
	std::string asmDir;
	for (int i = 1; i < a_argc; ++i) {
		const std::string arg = a_argv[i];
		if (arg == "--frames" && i + 1 < a_argc) {
			frames = std::atoi(a_argv[++i]);
		} else if (arg == "--seed" && i + 1 < a_argc) {
			seed = static_cast<uint32_t>(std::strtoul(a_argv[++i], nullptr, 10));
		} else if (arg == "--warp") {
			warp = true;
		} else if (arg == "--no-timing") {
			timing = false;
		} else if (arg == "--asm" && i + 1 < a_argc) {
			asmDir = a_argv[++i];
		} else {
			std::fprintf(stderr, "usage: ClipmapCSBench [--frames N] [--seed S] [--warp] [--no-timing] [--asm DIR]\n");
			return 2;
		}
	}

	try {
		const auto baseline = Compile(BaselineSource(), "ec04e21 shader");
		const auto current = Compile(CurrentSource(), "current shader");
		const auto baselineExact = Compile(WithoutNeighbourRace(BaselineSource()), "ec04e21 shader (race-free)");
		const auto currentExact = Compile(WithoutNeighbourRace(CurrentSource()), "current shader (race-free)");
		const auto listed = Compile(CurrentSource(), "current shader, GROUP_LIST", true);
		const auto listedExact = Compile(WithoutNeighbourRace(CurrentSource()), "current shader, GROUP_LIST (race-free)",
			true);
		const auto groupList = Compile(Clipmap::kGroupListShader, "group list pass");
		std::puts("PASS compiled ec04e21 and current update shaders, the GROUP_LIST variant and the group list pass "
				  "(cs_5_0, O3, MAX_STAMPS=64)");
		if (!asmDir.empty()) {
			WriteDisassembly(baseline.Get(), asmDir + "/clipmap_update_ec04e21.asm");
			WriteDisassembly(current.Get(), asmDir + "/clipmap_update_current.asm");
			WriteDisassembly(listed.Get(), asmDir + "/clipmap_update_listed.asm");
			WriteDisassembly(groupList.Get(), asmDir + "/clipmap_group_list.asm");
		}

		// The current shader runs with group lists (the default) unless noted.
		Gpu  gpu(warp);
		Side old;
		Side culled;
		gpu.CreateSide(old, baselineExact.Get());
		gpu.CreateSide(culled, currentExact.Get(), listedExact.Get(), groupList.Get(), true);
		RunFrames(gpu, old, culled, "bit-exact, repose on, neighbours from a pre-dispatch copy", frames, seed, true,
			true, true);
		RunFrames(gpu, old, culled, "bit-exact walks, repose on, neighbours from a pre-dispatch copy", frames,
			seed + 3, true, true, true, true);

		Side every;
		gpu.CreateSide(every, currentExact.Get(), listedExact.Get(), groupList.Get(), false);
		RunFrames(gpu, old, every, "bit-exact, every group (ClipmapSkipIdleGroups=0), repose on, pre-dispatch copy",
			std::min(frames, 120), seed + 4, true, true, true);

		Side oldShipping;
		Side culledShipping;
		gpu.CreateSide(oldShipping, baseline.Get());
		gpu.CreateSide(culledShipping, current.Get(), listed.Get(), groupList.Get(), true);
		RunFrames(gpu, oldShipping, culledShipping, "bit-exact, shipping shaders, repose off", frames, seed + 1,
			false, false, true);
		RunFrames(gpu, oldShipping, culledShipping, "bit-exact walks, shipping shaders, repose off", frames,
			seed + 5, false, false, true, true);

		// Lists broken on purpose must be caught on a walk with repose on. Fixed seeds, so
		// the result does not depend on --seed.
		{
			const std::string list = Clipmap::kGroupListShader;
			const std::string update = WithoutNeighbourRace(CurrentSource());
			struct Broken
			{
				const char* name;
				std::string update;
				std::string list;
			};
			const Broken broken[] = {
				{ "broken lists caught: no stamp rectangles", update,
					ReplaceOnce(list, "[branch] if (!listed) {", "[branch] if (false) {") },
				{ "broken lists caught: no ring", update,
					ReplaceOnce(list, "bool listed = any(wraps) || max(fromCentre.x, fromCentre.y) >= (int)Coarse.w;",
						"bool listed = false;") },
				{ "broken lists caught: no repose neighbours", update,
					ReplaceOnce(list, "LiveBefore[at] != 0 ||", "LiveBefore[at] != 0; bool unused = ") },
				{ "broken lists caught: -0.0 not live", ReplaceOnce(update, "(gBlockMax | gNegativeZero) != 0", "gBlockMax != 0"),
					list },
			};
			uint32_t brokenSeed = 1;
			for (const auto& b : broken) {
				Side side;
				gpu.CreateSide(side, Compile(b.update, b.name).Get(), Compile(b.update, b.name, true).Get(),
					Compile(b.list, b.name).Get(), true);
				RunFrames(gpu, old, side, b.name, 600, brokenSeed++, true, true, false, true, true);
			}
		}

		// With repose on, the shipping shader reads neighbours that other threads may
		// already have written. Show how far ec04e21 differs from itself run to run, and
		// how far the current shader differs from it, with lists and over every group.
		Side oldAgain;
		gpu.CreateSide(oldAgain, baseline.Get());
		Side everyShipping;
		gpu.CreateSide(everyShipping, current.Get(), listed.Get(), groupList.Get(), false);
		const struct
		{
			const char* name;
			Side*       side;
		} races[] = {
			{ "ec04e21 against itself", &oldAgain },
			{ "ec04e21 against current", &culledShipping },
			{ "ec04e21 against current, every group (ClipmapSkipIdleGroups=0)", &everyShipping },
		};
		for (const bool walk : { false, true }) {
			for (const auto& race : races) {
				const std::string name = std::string(race.name) + (walk ? ", walks" : "") + ", shipping, repose on";
				RunFrames(gpu, oldShipping, *race.side, name.c_str(), std::min(frames, 60), seed + 2, true, false, false,
					walk);
			}
		}

		if (!timing) {
			std::puts("Timing skipped (--no-timing)");
		} else if (warp) {
			std::puts("Timing skipped (WARP)");
		} else if (SkyrimRunning()) {
			std::puts("Timing skipped: SkyrimSE.exe is running on this GPU");
		} else {
			Timing(gpu, oldShipping, culledShipping);
			ScheduleTiming(gpu, oldShipping, everyShipping, culledShipping);
		}

		std::puts("\nALL PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
