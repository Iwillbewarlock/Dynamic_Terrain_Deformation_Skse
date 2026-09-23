// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <string_view>
#include "ClipmapUpdateCS.h"
#include "ImpactPatterns.h"
#include "ShelterTransition.h"
#include "BlanketTessellation.h"
#include "TessellationResources.h"
#include "BloodDecalFilter.h"
#include "ObjectStampFilter.h"
#include "TerrainDepthBias.h"
#include "TerrainCulling.h"
#include <limits>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;
constexpr UINT N = 64;
struct Params
{
	float window[4]{ 0, 0, 1, N };
	float control[4]{ 0, 1, N / 2 - 2, 0.55f };
	float weather[4]{ 0, 0, 0, 0.4f };
	float stamps[Clipmap::kMaxStamps][4]{};
	float stampParams[Clipmap::kMaxStamps][4]{};
	float stampShape[Clipmap::kMaxStamps][4]{};
	float stampMotion[Clipmap::kMaxStamps][4]{};
	float coarse[4]{ 0, N, 0, N / 2 - 2 };
	float rimShape[4]{ 0.5f, 0.45f, 0, 0 };
	float snowRim[4]{ 0.55f, 0.4f, 0.5f, 0.45f };

	float raise[4]{ 0, 1, 0, 0 };
	float raiseWindow[4]{ 0, 0, 1.0e6f, 2.0e6f };
	float stampBounds[Clipmap::kMaxStamps][4]{};
	Params()
	{
		stamps[0][2] = 5;
		stamps[0][3] = 2;
		stampParams[0][0] = 0.3f;
		stampParams[0][1] = 0.92f;
		stampParams[0][3] = 1;
	}
};
void Require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

class Field
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11Texture2D> height, metadata, activity, readHeight, readMeta;
	ComPtr<ID3D11UnorderedAccessView> heightUAV, metaUAV, activityUAV;
	ComPtr<ID3D11ShaderResourceView> coarseHeight, coarseMeta;
	ComPtr<ID3D11Texture2D> coverage, meshCap;
	ComPtr<ID3D11ShaderResourceView> coverageSRV, meshCapSRV;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
public:
	Field()
	{
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
			D3D11_SDK_VERSION, &device, nullptr, &context), "Create WARP device");
		ComPtr<ID3DBlob> code, errors;
		const auto source = Clipmap::UpdateShaderSource();
		const auto count = std::to_string(Clipmap::kMaxStamps);
		const D3D_SHADER_MACRO defines[]{ { "MAX_STAMPS", count.c_str() }, {} };
		const auto hr = D3DCompile(source.data(), source.size(), "StampSurfaceTest", defines,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
		Check(hr, "Compile shipping stamp shader");
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "Create shader");
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = N;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		Check(device->CreateTexture2D(&desc, nullptr, &height), "Create height");
		Check(device->CreateUnorderedAccessView(height.Get(), nullptr, &heightUAV), "Height UAV");
		desc.Format = DXGI_FORMAT_R8G8_UNORM;
		Check(device->CreateTexture2D(&desc, nullptr, &metadata), "Create metadata");
		Check(device->CreateUnorderedAccessView(metadata.Get(), nullptr, &metaUAV), "Metadata UAV");
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.Width = desc.Height = Clipmap::kActivityTexels;
		Check(device->CreateTexture2D(&desc, nullptr, &activity), "Create activity");
		Check(device->CreateUnorderedAccessView(activity.Get(), nullptr, &activityUAV), "Activity UAV");
		desc.Width = desc.Height = N;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		Check(device->CreateTexture2D(&desc, nullptr, &readHeight), "Height readback");
		desc.Format = DXGI_FORMAT_R8G8_UNORM;
		Check(device->CreateTexture2D(&desc, nullptr, &readMeta), "Metadata readback");
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = sizeof(Params);
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&buffer, nullptr, &cb), "Params buffer");
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sd, &sampler), "Sampler");

		D3D11_TEXTURE2D_DESC map{};
		map.Width = map.Height = 4;
		map.MipLevels = map.ArraySize = map.SampleDesc.Count = 1;
		map.Format = DXGI_FORMAT_R32_FLOAT;
		map.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		Check(device->CreateTexture2D(&map, nullptr, &coverage), "Create coverage");
		Check(device->CreateTexture2D(&map, nullptr, &meshCap), "Create mesh cap");
		Check(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageSRV), "Coverage SRV");
		Check(device->CreateShaderResourceView(meshCap.Get(), nullptr, &meshCapSRV), "Mesh cap SRV");
		SetFloorMaps(1.0f, 1.0f);
		Clear();
	}
	void CheckTerrainCulling()
	{
		TerrainCulling::Bracket bracket;
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_WIREFRAME; desc.CullMode = D3D11_CULL_NONE;
		desc.FrontCounterClockwise = TRUE; desc.DepthBias = -9;
		desc.DepthBiasClamp = 0.5f; desc.SlopeScaledDepthBias = -1.2f;
		desc.DepthClipEnable = TRUE; desc.ScissorEnable = TRUE;
		desc.MultisampleEnable = TRUE;
		ComPtr<ID3D11RasterizerState> none, front;
		Check(device->CreateRasterizerState(&desc, &none), "Create no-cull test state");
		Require(bracket.Select(device.Get(), none.Get()) == none.Get(), "Inactive bracket preserves non-terrain state");
		context->RSSetState(nullptr);
		bracket.Begin(context.Get());
		auto* selected = bracket.Select(device.Get(), none.Get());
		Require(selected && selected != none.Get(), "No-cull terrain gets replacement");
		D3D11_RASTERIZER_DESC actual{}; selected->GetDesc(&actual);
		Require(actual.CullMode == D3D11_CULL_BACK && actual.FrontCounterClockwise == desc.FrontCounterClockwise &&
			actual.FillMode == desc.FillMode && actual.DepthBias == desc.DepthBias && actual.DepthBiasClamp == desc.DepthBiasClamp &&
			actual.SlopeScaledDepthBias == desc.SlopeScaledDepthBias && actual.DepthClipEnable == desc.DepthClipEnable &&
			actual.ScissorEnable == desc.ScissorEnable && actual.MultisampleEnable == desc.MultisampleEnable &&
			actual.AntialiasedLineEnable == desc.AntialiasedLineEnable, "Only cull mode changes");
		Require(bracket.Select(device.Get(), none.Get()) == selected, "Equivalent request reuses replacement");
		context->RSSetState(selected);
		bracket.End(context.Get());
		ComPtr<ID3D11RasterizerState> restored; context->RSGetState(&restored);
		Require(restored.Get() == none.Get(), "Restore latest renderer request, not initial null");
		desc.CullMode = D3D11_CULL_FRONT;
		Check(device->CreateRasterizerState(&desc, &front), "Create front-cull test state");
		bracket.Begin(context.Get());
		Require(bracket.Select(device.Get(), front.Get()) == front.Get(), "Intentional front-face culling preserved");
		context->RSSetState(bracket.Select(device.Get(), nullptr));
		bracket.End(context.Get());
		restored.Reset(); context->RSGetState(&restored);
		Require(!restored, "Restore late null/default request exactly");
		Require(bracket.Select(device.Get(), none.Get()) == none.Get(), "Subsequent object draw remains unmodified");
		bracket.Reset();
		std::puts("PASS terrain culling scope, descriptor preservation, cache and latest-state restoration");
	}

	void CheckTessellationResources()
	{
		ComPtr<ID3D11ShaderResourceView> view;
		Check(device->CreateShaderResourceView(height.Get(), nullptr, &view), "State test SRV");
		for (unsigned phase = 0; phase < 2; ++phase) {

			for (UINT slot = 0; slot < 14; ++slot) {
				auto* value = (slot + phase) % 2 ? cb.Get() : nullptr;
				context->DSSetConstantBuffers(slot, 1, &value);
				context->HSSetConstantBuffers(slot, 1, &value);
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				auto* srv = (slot + phase) % 2 ? view.Get() : nullptr;
				auto* state = (slot + phase) % 2 ? sampler.Get() : nullptr;
				context->DSSetShaderResources(slot, 1, &srv);
				context->HSSetShaderResources(slot, 1, &srv);
				context->DSSetSamplers(slot, 1, &state);
			}
			Tessellation::SavedResources saved;
			saved.Capture(context.Get());
			for (UINT slot = 10; slot < 14; ++slot) {
				auto* value = (slot + phase) % 2 ? nullptr : cb.Get();
				context->DSSetConstantBuffers(slot, 1, &value);
				if (slot >= 12) { context->HSSetConstantBuffers(slot, 1, &value); }
			}
			for (UINT slot = 0; slot < 4; ++slot) {
				auto* srv = (slot + phase) % 2 ? nullptr : view.Get();
				auto* state = (slot + phase) % 2 ? nullptr : sampler.Get();
				context->DSSetShaderResources(slot, 1, &srv);
				if (slot == 0) { context->HSSetShaderResources(slot, 1, &srv); }
				if (slot < 3) { context->DSSetSamplers(slot, 1, &state); }
			}
			saved.Restore(context.Get());
			for (UINT slot = 0; slot < 14; ++slot) {
				ComPtr<ID3D11Buffer> domain, hull;
				context->DSGetConstantBuffers(slot, 1, &domain);
				context->HSGetConstantBuffers(slot, 1, &hull);
				auto* expected = (slot + phase) % 2 ? cb.Get() : nullptr;
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS constant buffers");
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				ComPtr<ID3D11ShaderResourceView> domain, hull;
				ComPtr<ID3D11SamplerState> state;
				context->DSGetShaderResources(slot, 1, &domain);
				context->HSGetShaderResources(slot, 1, &hull);
				context->DSGetSamplers(slot, 1, &state);
				auto* expected = (slot + phase) % 2 ? view.Get() : nullptr;
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS resources");
				Require(state.Get() == ((slot + phase) % 2 ? sampler.Get() : nullptr), "Restore DS samplers");
			}
		}
		context->ClearState();
		std::puts("PASS tessellation resource restoration: occupied/null slots and untouched neighbours");
	}
	void CheckBlanketBudget()
	{
		const std::string source = std::string(BlanketTessellation::source) + R"(
RWTexture2D<float> Result : register(u0);
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    float values[8] = {
        BlanketDetail(128, 2, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, 1),
        BlanketDetail(128, 2, 64, 8, 64, 0.5),
        BlanketDetail(128, 2, 64, 0, 64, 0),
        BlanketDetail(128, 2, 8, 8, 8, 0),
        BlanketDetail(128, 2, 4, 8, 64, 0),
        BlanketDetail(128, 32, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, -1)
    };
    Result[id.xy] = values[id.x];
})";
		ComPtr<ID3DBlob> code, errors;
		Check(D3DCompile(source.data(), source.size(), "BlanketBudgetTest", nullptr,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile blanket budget");
		ComPtr<ID3D11ComputeShader> budget;
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &budget), "Create blanket budget shader");
		context->CSSetShader(budget.Get(), nullptr, 0);
		context->CSSetUnorderedAccessViews(0, 1, heightUAV.GetAddressOf(), nullptr);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* empty = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
		const auto result = Read();
		const float expected[]{16, 64, 40, 2, 8, 4, 32, 16};
		for (size_t i = 0; i < 8; ++i) {
			Require(std::abs(result[i] - expected[i]) < 0.001f, "Blanket floor, detail limits and activity regression");
		}
		Clear();
		std::puts("PASS GPU blanket budget: empty snow, active marks, interpolation, off, caps and finer user baseline");
	}

	void SetFloorMaps(float a_cover, float a_cap)
	{
		const float cover[16]{ a_cover, a_cover, a_cover, a_cover, a_cover, a_cover,
			a_cover, a_cover, a_cover, a_cover, a_cover, a_cover, a_cover, a_cover,
			a_cover, a_cover };
		const float cap[16]{ a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap,
			a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap };
		context->UpdateSubresource(coverage.Get(), 0, nullptr, cover, 4 * sizeof(float), 0);
		context->UpdateSubresource(meshCap.Get(), 0, nullptr, cap, 4 * sizeof(float), 0);
	}
	void Clear()
	{
		const float zero[4]{};
		context->ClearUnorderedAccessViewFloat(heightUAV.Get(), zero);
		context->ClearUnorderedAccessViewFloat(metaUAV.Get(), zero);
	}
	void Step(const Params& params, bool seed = false)
	{
		Params filled = params;
		Clipmap::FillStampBounds(filled, Clipmap::kMaxStamps);
		context->UpdateSubresource(cb.Get(), 0, nullptr, &filled, 0, 0);
		ID3D11UnorderedAccessView* uavs[]{ heightUAV.Get(), metaUAV.Get(), activityUAV.Get() };
		ID3D11ShaderResourceView* srvs[]{ nullptr, seed ? coarseHeight.Get() : nullptr,
			seed ? coarseMeta.Get() : nullptr, coverageSRV.Get(), meshCapSRV.Get() };
		context->CSSetShader(shader.Get(), nullptr, 0);
		context->CSSetShaderResources(0, 5, srvs);
		context->CSSetSamplers(0, 1, sampler.GetAddressOf());
		context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->Dispatch(N / 8, N / 8, 1);
		ID3D11UnorderedAccessView* nullUAVs[3]{};
		ID3D11ShaderResourceView* nullSRVs[5]{};
		context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
		context->CSSetShaderResources(0, 5, nullSRVs);
	}
	std::vector<float> Read()
	{
		context->CopyResource(readHeight.Get(), height.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readHeight.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read height");
		std::vector<float> result(N * N);
		for (UINT y = 0; y < N; ++y) {
			const auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
			std::copy_n(row, N, result.begin() + y * N);
		}
		context->Unmap(readHeight.Get(), 0);
		return result;
	}
	unsigned char SnowAtOrigin()
	{
		context->CopyResource(readMeta.Get(), metadata.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readMeta.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read metadata");
		const auto snow = static_cast<const unsigned char*>(mapped.pData)[1];
		context->Unmap(readMeta.Get(), 0);
		return snow;
	}
	void SaveCoarse()
	{
		D3D11_TEXTURE2D_DESC desc{};
		for (int i = 0; i < 2; ++i) {
			auto* source = i ? metadata.Get() : height.Get();
			source->GetDesc(&desc);
			ComPtr<ID3D11Texture2D> copy;
			Check(device->CreateTexture2D(&desc, nullptr, &copy), "Coarse copy");
			context->CopyResource(copy.Get(), source);
			Check(device->CreateShaderResourceView(copy.Get(), nullptr,
				i ? &coarseMeta : &coarseHeight), "Coarse SRV");
		}
	}
};

void CheckGroundFloor(Field& field)
{
	const auto deepest = [](const std::vector<float>& a_field) {
		return *std::min_element(a_field.begin(), a_field.end());
	};

	Params p;
	p.stampMotion[0][2] = 1;
	p.stamps[0][3] = 40.0f;
	field.SetFloorMaps(1.0f, 1.0f);

	field.Clear(); field.Step(p);
	const auto unfloored = field.Read();
	Require(deepest(unfloored) < -20.0f, "The test stamp must overshoot every blanket below");

	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	const float floored = deepest(field.Read());
	Require(floored > -10.001f, "A snow mark cut below the blanket standing over it");
	Require(floored < -9.999f, "The floor is not the blanket - the mark stopped short of the land");

	p.raise[2] = 3.0f;
	field.Clear(); field.Step(p);
	const float bitten = deepest(field.Read());
	Require(bitten > -13.001f && bitten < -12.999f, "SnowGroundBite is not the floor's slack");
	p.raise[2] = 0.0f;

	p.raise[3] = 1.0f;
	field.SetFloorMaps(1.0f, 0.5f);
	field.Clear(); field.Step(p);
	const float capped = deepest(field.Read());
	Require(capped > -5.001f && capped < -4.999f, "The mesh cap did not carry into the floor");
	p.raise[3] = 0.0f;

	field.SetFloorMaps(0.0f, 1.0f);
	field.Clear(); field.Step(p);
	Require(deepest(field.Read()) > -0.001f, "Ground with no blanket still took a snow cut");
	field.SetFloorMaps(1.0f, 1.0f);

	p.stampMotion[0][2] = 0;
	p.raise[0] = 0.0f;
	field.Clear(); field.Step(p);
	const auto mud = field.Read();
	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	Require(field.Read() == mud, "The floor reached ground that no snow stamp claimed");

	field.Clear();
	std::puts("PASS snow ground floor: blanket, bite, mesh cap, bare ground and other surfaces");
}

void CheckMagicPatterns(Field& field)
{
	const auto draw = [&](const ImpactPatterns::Pattern& pattern, bool snow, float span, float lean) {
		Params p;
		p.control[1] = static_cast<float>(pattern.count);
		p.control[3] = span; p.rimShape[0] = lean;
		p.snowRim[0] = span; p.snowRim[2] = lean;
		for (size_t i = 0; i < pattern.count; ++i) {
			const auto& s = pattern.strokes[i];
			p.stamps[i][0] = s.x; p.stamps[i][1] = s.y;
			p.stamps[i][2] = ImpactPatterns::StampRadius(s.radius, 1.0f, span, lean);
			p.stamps[i][3] = s.strength * 3.0f;
			p.stampParams[i][0] = 0.25f; p.stampParams[i][1] = 0.92f;
			p.stampParams[i][3] = s.strength * 0.8f;
			p.stampMotion[i][0] = s.motionX; p.stampMotion[i][1] = s.motionY;
			p.stampMotion[i][2] = snow ? 1.0f : 0.0f;
		}
		field.Clear(); field.Step(p);
		const auto first = field.Read();
		field.Step(p);
		Require(field.Read() == first, "Repeated impact must not deepen or accumulate rims");
		return first;
	};
	Require(ImpactPatterns::BoundedRadius(100000, 2, 256) == 256, "Explosion radius cap failed");
	Require(ImpactPatterns::BoundedRadius(std::numeric_limits<float>::infinity(), 1, 256) == 0,
		"Non-finite explosion radius must be rejected");
	Require(ImpactPatterns::Directional(20, 8, 0, 0, true).count == 0 &&
		ImpactPatterns::Directional(0, 8, 0, 1, true).count == 0, "Invalid direction/range must not stamp");
	for (bool snow : { false, true }) {
		for (float span : { 0.0f, 0.8f, 4.0f }) {
			const auto explosion = draw(ImpactPatterns::Explosion(20), snow, span, 1);
			Require(*std::min_element(explosion.begin(), explosion.end()) < -0.1f,
				"Explosion must deform both land and snow");
			for (int y = 0; y < N; ++y) {
				for (int x = 0; x < N; ++x) {
					const float value = explosion[y * N + x];
					Require(std::isfinite(value), "Impact generated non-finite displacement");
					const int wx = x < N/2 ? x : x - N, wy = y < N/2 ? y : y - N;
					if (std::hypot(static_cast<float>(wx), static_cast<float>(wy)) > 21) {
						Require(value == 0, "Explosion affected ground beyond radius cap");
					}
				}
			}
		}
		const auto forward = draw(ImpactPatterns::Directional(20, 10, 0, 1, true), snow, 0.8f, 1);
		Require(forward[14 * N] < -0.1f && forward[(N - 10) * N] == 0,
			"Shout must disturb ground ahead, not behind");
		const auto sideways = draw(ImpactPatterns::Directional(20, 10, 1, 0, true), snow, 0.8f, 1);
		Require(sideways[14] < -0.1f && sideways[14 * N] == 0,
			"Shout pattern did not turn with casting direction");
	}
	std::puts("PASS magic impacts: land/snow, directional shouts, radius caps and idempotence");
}

int main(int argc, char** argv)
{
	try {
		float at30 = 255.0f, at144 = 255.0f;
		for (int i = 0; i < 30; ++i) { ShelterTransition::Advance(at30, 0, ShelterTransition::Alpha(1.0f / 30)); }
		for (int i = 0; i < 144; ++i) { ShelterTransition::Advance(at144, 0, ShelterTransition::Alpha(1.0f / 144)); }
		Require(std::abs(at30 - at144) < 0.001f, "Shelter transition must be frame-rate independent");
		Require(at30 > 0 && at30 < 20, "Shelter must approach the target without snapping");
		const float shelterBefore = at30;
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(0));
		Require(at30 == shelterBefore, "Paused shelter must not move");
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(1.0f / 60));
		Require(at30 > shelterBefore && at30 < 255, "Reversing a ray answer must not snap or overshoot");
		Require(ShelterTransition::Alpha(std::numeric_limits<float>::quiet_NaN()) == 0, "Invalid delta must be ignored");
		std::puts("PASS shelter transitions: frame rate, pause, reversal and invalid time");
		Require(BloodDecalFilter::Matches("textures/effects/BloodSplatter01.DDS", " blood "), "Blood basename matching");
		Require(BloodDecalFilter::Matches("custom/EBT_pool.dds", "blood,ebt_"), "Custom blood prefix");
		Require(!BloodDecalFilter::Matches("blood/scorch.dds", "blood"), "Blood directory is not identification");
		Require(!BloodDecalFilter::Matches("blood.dds", " , "), "Empty prefixes match nothing");
		Require(!BloodDecalFilter::Matches("arrow.dds", "blood"), "Non-blood decal isolation");
		const auto groundBloodPrefixes = "blood,decalsblood,bigspatter";
		Require(BloodDecalFilter::Matches("SanguineSymphony/Effects/Default/BloodSprayGroundImpactRegular.dds", groundBloodPrefixes), "Sanguine ground spray");
		Require(BloodDecalFilter::Matches("Blood/DecalsBloodSplatterBlend01.dds", groundBloodPrefixes), "Native ground splatter");
		Require(BloodDecalFilter::Matches("blood/bigspatter01.dds", groundBloodPrefixes), "Rip n Tear splatter");
		Require(!BloodDecalFilter::Matches("ImpactDecals/DecalFlameBurn01.dds", groundBloodPrefixes), "Observed scorch must not become blood");
		Require(!BloodDecalFilter::Matches("SanguineSymphony/Blood/Default/BladeCutLarge.dds", groundBloodPrefixes), "Wound texture is not a ground spray");
		std::puts("PASS blood texture identification and non-blood isolation");
		Require(TerrainDepthBias::Fingerprint("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 standard vector");
        Require(TerrainDepthBias::Instructions("// header\r\nvs_5_0  \r\nret\r\n// footer\n") == "vs_5_0\nret\n",
            "Shader normalization excludes comments and normalizes whitespace");
        Require(TerrainDepthBias::Recognize("vs_5_0\nret\n") == 0, "Unknown shaders remain uncorrected");
        Require(TerrainDepthBias::Recognize("") == 0, "Missing shader remains uncorrected");

        if (argc > 1) {
            std::ifstream file(argv[1], std::ios::binary);
            Require(file.is_open(), "Local shader capture opens");
            const std::string captured{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            Require(TerrainDepthBias::Recognize(captured) == 10.0f, "Captured offset shader fingerprint matches");
            auto changed = captured;
            const auto offset = changed.find("10.000000");
            Require(offset != std::string::npos, "Local capture has expected offset");
            changed.replace(offset, 9, "5.000000");
            Require(TerrainDepthBias::Recognize(changed) == 0, "Different offset remains uncorrected");
            changed = captured;
            const auto matrix = changed.find("cb12[10]");
            Require(matrix != std::string::npos, "Local capture has expected matrix");
            changed.replace(matrix, 8, "cb12[20]");
            Require(TerrainDepthBias::Recognize(changed) == 0, "Different matrix remains uncorrected");
            Require(TerrainDepthBias::Recognize("// reflection\n" + captured + "// footer\n") == 10.0f,
                "Reflection comments do not change recognition");
            std::puts("PASS local terrain shader fingerprint and variant isolation");
        }

		for (double nearPlane : { 5.0, 10.0, 15.0 }) {
			const double farPlane = 10000.0, A = farPlane / (farPlane - nearPlane), B = -nearPlane * A;
			for (double z : { 30.0, 300.0, 3000.0 }) {
				const double clipZ = A * z + B, biasedZ = clipZ + 10.0;
				const double inverseW = ((biasedZ - 10.0) - A * z) / B;
				Require(std::abs(z / inverseW - z) < 0.00001, "Corrected world depth agrees with unbiased pass");
				Require(std::abs(123.0 / inverseW - 123.0) < 0.00001, "Corrected world XY agrees with unbiased pass");
				Require(std::abs(((biasedZ + A * 35.0) - (clipZ + A * 35.0)) - 10.0) < 0.00001, "Raster depth bias survives displacement");
			}
		}
		std::puts("PASS captured terrain depth shader isolation and biased projection reconstruction");
		Require(ObjectStampFilter::IsVisualBloodProjectile("SanguineSymphony\\Effects\\Default\\BloodSprayProjectileRegular.nif"), "Blood spray must not stamp snow");
		Require(ObjectStampFilter::IsVisualBloodProjectile("SanguineSymphony/Effects/Insect/BLOODSPRAYPROJECTILESUBTLE.NIF"), "Insect spray and case handling");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("weapons/iron/ironarrow.nif"), "Arrow stamps preserved");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("BloodSprayProjectileRegular/ironarrow.nif"), "Spray directory must not exclude solid projectile");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("magic/fireball.nif"), "Unrelated projectiles preserved");
		Require(!ObjectStampFilter::IsVisualBloodProjectile(""), "Missing model keeps existing classification");
		std::puts("PASS visual blood projectile exclusion and ordinary projectile preservation");
		Field field;
		field.CheckTessellationResources();
		field.CheckTerrainCulling();
		field.CheckBlanketBudget();
		CheckMagicPatterns(field);
		CheckGroundFloor(field);
		field.Clear();
		Params p;
		field.Step(p);
		const auto ordinary = field.Read();
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		Require(field.Read() == ordinary, "Inherited snow values must match global geometry");
		Require(field.SnowAtOrigin() == 255, "Snow classification must be written");
		std::puts("PASS inherited snow settings preserve global geometry");
		p.control[3] = 0; p.weather[3] = 0; p.rimShape[1] = 0;
		p.snowRim[0] = 1;
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		const auto noRim = field.Read();
		Require(*std::max_element(noRim.begin(), noRim.end()) == 0, "Global zero span must suppress other-ground rim");
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		const auto snowRim = field.Read();
		Require(*std::max_element(snowRim.begin(), snowRim.end()) > 0.1f, "Snow span must work with global span zero");
		std::puts("PASS independent snow rim span, including global span zero");
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		p.snowRim[1] = 1; p.snowRim[2] = 1; p.snowRim[3] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read() == noRim, "Snow noise/lean/churn leaked into non-snow stamp");
		std::puts("PASS snow noise/lean/churn do not alter other ground");
		for (int parameter = 1; parameter <= 3; ++parameter) {
			p = Params{};
			field.Clear(); field.Step(p);
			const auto baseline = field.Read();
			p.snowRim[parameter] = parameter == 2 ? 1.0f : 0.0f;
			field.Clear(); field.Step(p);
			Require(field.Read() == baseline, "Snow override changed a non-snow rim");
			p.stampMotion[0][2] = 1;
			field.Clear(); field.Step(p);
			Require(field.Read() != baseline, "Snow override has no effect on snow");
		}
		std::puts("PASS each noise/lean/churn override changes only snow with both rims active");
		p = Params{}; p.stampMotion[0][2] = 1;
		p.snowRim[0] = 1; p.snowRim[1] = 0; p.snowRim[2] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read()[12] > 0.1f, "Outward lean rim was clipped by early bounds");
		std::puts("PASS wide outward rims survive the stamp bounds check");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p);
		const auto before = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.weather[2] = 0; p.rimShape[2] = 1;
		field.Step(p);
		Require(field.Read() != before && field.SnowAtOrigin() == 255,
			"Snow must keep its own repose rate after the stamp leaves");
		p = Params{}; field.Clear(); field.Step(p);
		const auto nonSnow = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.weather[2] = 0; p.rimShape[2] = 1;
		field.Step(p);
		Require(field.Read() == nonSnow, "Snow repose rate affected old non-snow marks");
		std::puts("PASS persistent snow/non-snow repose isolation");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p); field.SaveCoarse(); field.Clear();
		p.control[1] = 0; p.coarse[0] = 1; p.coarse[3] = 0;
		field.Step(p, true);
		Require(field.SnowAtOrigin() == 255 && field.Read()[0] < 0,
			"Coarse seeding lost mark or snow classification");
		std::puts("PASS coarse-to-fine seeding retains snow classification");
		std::puts("ALL STAMP SURFACE TESTS PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
