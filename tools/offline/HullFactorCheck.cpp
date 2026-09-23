// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN) and contributors.

// Runs the generated landscape hull and domain shaders on WARP and compares the hull
// savings (frustum cull, screen cap, factor snap, canonical edges) with the legacy
// generator output. The scene is 128-unit land triangles drawn one 2048-unit quadrant
// per draw with camera-relative float offsets, far from the world origin, with a
// trail in the activity grid. "all on" includes the screen cap; "shipped" is the INI
// default (cull, snap and canonical edges, cap off).
//
//  A) Factor readback: a pass-through domain shader writes pc.* to a UAV, culled
//     patches keep a -1 sentinel.
//     - shared edges between drawn patches (also across quadrant draws) get the same
//       integer factor
//     - no factor above legacy anywhere
//     - factors do not change under 8 sub-pixel projection jitters
//     - pipeline statistics per pass (DS invocations, generated and clipped primitives)
//  B) Depth images with the real depth-prepass domain shader over a torture field
//     (craters -120u, rims +60u, snow raise +35u, hills): cull on and cull off must be
//     bit-identical, with the screen cap on and with the shipped settings.
//
//   HullFactorCheck [--jitter]
//
// --jitter runs 1080p only and skips the depth images. WARP is slow: the quick run takes
// several minutes and the full run about twice as long.

#include "PCH.h"

#include "ShaderReflection.cpp"
#include "Tessellation.cpp"

namespace ShaderRegistry
{
	Bytecode For(ID3D11VertexShader*) { return {}; }
}

#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

namespace globals
{
	bool Ready()
	{
		return false;
	}
}

namespace Clipmap
{
	bool Ready()
	{
		return true;
	}

	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace SnowCoverage
{
	bool Ready() { return true; }
	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace Shelter
{
	bool Ready() { return true; }
	bool Initialize() { return true; }
	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace Profiler
{
	void    GpuBegin(Scope) {}
	void    GpuEnd() {}
	void    Tally(Count, uint32_t) {}
	int64_t Ticks() { return 0; }
	void    AddCpuTicks(CpuScope, int64_t) {}
}

namespace
{
	ID3D11Device*        g_device{ nullptr };
	ID3D11DeviceContext* g_context{ nullptr };

	ID3DBlob* Compile(const std::string& a_source, const char* a_target)
	{
		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		if (FAILED(D3DCompile(a_source.c_str(), a_source.size(), a_target, nullptr, nullptr,
				"main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
			std::printf("compile %s failed:\n%s\n", a_target,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			std::exit(2);
		}
		if (errors) {
			errors->Release();
		}
		return code;
	}

	struct Mat
	{
		double m[4][4];
	};

	Mat Mul(const Mat& a_a, const Mat& a_b)
	{
		Mat r{};
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				for (int k = 0; k < 4; ++k) {
					r.m[i][j] += a_a.m[i][k] * a_b.m[k][j];
				}
			}
		}
		return r;
	}

	bool Invert(const Mat& a_in, Mat& a_out)
	{
		double m[4][8];
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 8; ++j) {
				m[i][j] = j < 4 ? a_in.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
			}
		}
		for (int c = 0; c < 4; ++c) {
			int p = c;
			for (int r = c + 1; r < 4; ++r) {
				if (std::fabs(m[r][c]) > std::fabs(m[p][c])) {
					p = r;
				}
			}
			if (std::fabs(m[p][c]) < 1e-12) {
				return false;
			}
			for (int j = 0; j < 8; ++j) {
				std::swap(m[c][j], m[p][j]);
			}
			const double d = m[c][c];
			for (int j = 0; j < 8; ++j) {
				m[c][j] /= d;
			}
			for (int r = 0; r < 4; ++r) {
				if (r == c) {
					continue;
				}
				const double f = m[r][c];
				for (int j = 0; j < 8; ++j) {
					m[r][j] -= f * m[c][j];
				}
			}
		}
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				a_out.m[i][j] = m[i][j + 4];
			}
		}
		return true;
	}

	struct Camera
	{
		double pos[3], right[3], up[3], fwd[3];
		double P00, P11;
	};

	Camera MakeCamera(double a_x, double a_y, double a_z, double a_yawDeg, double a_pitchDeg,
		double a_P11, double a_aspect)
	{
		const double yaw = a_yawDeg * 3.14159265358979 / 180.0;
		const double pitch = a_pitchDeg * 3.14159265358979 / 180.0;

		Camera c{};
		c.pos[0] = a_x;
		c.pos[1] = a_y;
		c.pos[2] = a_z;
		c.fwd[0] = std::sin(yaw) * std::cos(pitch);
		c.fwd[1] = std::cos(yaw) * std::cos(pitch);
		c.fwd[2] = std::sin(pitch);
		c.right[0] = std::cos(yaw);
		c.right[1] = -std::sin(yaw);
		c.right[2] = 0.0;
		c.up[0] = c.right[1] * c.fwd[2] - c.right[2] * c.fwd[1];
		c.up[1] = c.right[2] * c.fwd[0] - c.right[0] * c.fwd[2];
		c.up[2] = c.right[0] * c.fwd[1] - c.right[1] * c.fwd[0];
		c.P11 = a_P11;
		c.P00 = a_P11 / a_aspect;
		return c;
	}

	// clip = M * (p, 1). TAA/DLSS jitter is j * w added to clip x and y (off-centre projection).
	Mat ViewProj(const Camera& a_camera, double a_jx, double a_jy)
	{
		const double n = 15.0;
		const double f = 353000.0;

		Mat V{};
		for (int j = 0; j < 3; ++j) {
			V.m[0][j] = a_camera.right[j];
			V.m[1][j] = a_camera.up[j];
			V.m[2][j] = a_camera.fwd[j];
		}
		for (int r = 0; r < 3; ++r) {
			const double* axis = r == 0 ? a_camera.right : r == 1 ? a_camera.up : a_camera.fwd;
			V.m[r][3] = -(axis[0] * a_camera.pos[0] + axis[1] * a_camera.pos[1] +
						  axis[2] * a_camera.pos[2]);
		}
		V.m[3][3] = 1.0;

		Mat P{};
		P.m[0][0] = a_camera.P00;
		P.m[0][2] = a_jx;
		P.m[1][1] = a_camera.P11;
		P.m[1][2] = a_jy;
		P.m[2][2] = f / (f - n);
		P.m[2][3] = -n * f / (f - n);
		P.m[3][2] = 1.0;
		return Mul(P, V);
	}

	using Clip4 = std::array<double, 4>;

	Clip4 Clip(const Mat& a_m, double a_x, double a_y, double a_z)
	{
		Clip4 r{};
		for (int i = 0; i < 4; ++i) {
			r[i] = a_m.m[i][0] * a_x + a_m.m[i][1] * a_y + a_m.m[i][2] * a_z + a_m.m[i][3];
		}
		return r;
	}

	// Engine-style quadrant cull: every corner outside the same frustum plane.
	bool OutsideSame(const std::vector<Clip4>& a_clips)
	{
		const auto all = [&](auto a_outside) {
			for (const auto& c : a_clips) {
				if (!a_outside(c)) {
					return false;
				}
			}
			return true;
		};
		return all([](const Clip4& c) { return c[0] < -c[3]; }) ||
		       all([](const Clip4& c) { return c[0] > c[3]; }) ||
		       all([](const Clip4& c) { return c[1] < -c[3]; }) ||
		       all([](const Clip4& c) { return c[1] > c[3]; }) ||
		       all([](const Clip4& c) { return c[3] < 15.0; });
	}

	ID3D11Buffer* MakeConstants(const void* a_data, UINT a_size)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_size;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA initial{ a_data };
		ID3D11Buffer* buffer = nullptr;
		g_device->CreateBuffer(&desc, &initial, &buffer);
		return buffer;
	}

	ID3D11ShaderResourceView* MakeTexture(DXGI_FORMAT a_format, UINT a_size, const void* a_data,
		UINT a_pitch)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_size;
		desc.Height = a_size;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA initial{ a_data, a_pitch };
		ID3D11Texture2D* texture = nullptr;
		g_device->CreateTexture2D(&desc, &initial, &texture);
		ID3D11ShaderResourceView* view = nullptr;
		g_device->CreateShaderResourceView(texture, nullptr, &view);
		texture->Release();
		return view;
	}

	double Hill(double a_x, double a_y)
	{
		return 350.0 * std::sin(a_x / 1700.0) * std::cos(a_y / 1300.0) +
		       120.0 * std::sin((a_x + a_y) / 600.0);
	}

	constexpr double kBaseX = 61440.0;
	constexpr double kBaseY = -40960.0;

	using GridPoint = std::pair<int, int>;

	struct Scene
	{
		double playerX{ kBaseX + 1300.0 };
		double playerY{ kBaseY + 900.0 };
		bool   hills{ false };
		float  posAdjust[3]{};

		std::vector<float>                    verts;
		std::vector<std::array<GridPoint, 3>> ids;

		struct DrawRange
		{
			UINT  first;
			UINT  count;
			float offset[4];
		};
		std::vector<DrawRange> draws;
	};

	// One draw per 2048u quadrant with camera-relative float offsets, like the game's
	// per-geometry transforms, and the engine-style quadrant cull. 128u land triangles.
	void BuildScene(Scene& a_scene, const Mat& a_cullViewProj)
	{
		a_scene.verts.clear();
		a_scene.ids.clear();
		a_scene.draws.clear();

		const double spacing = 128.0;
		const double extent = 10240.0;
		const double cx = std::floor(a_scene.playerX / 0.75) * 0.75;
		const double cy = std::floor(a_scene.playerY / 0.75) * 0.75;
		const int    q0x = static_cast<int>(std::floor((a_scene.playerX - extent) / 2048.0));
		const int    q1x = static_cast<int>(std::floor((a_scene.playerX + extent) / 2048.0));
		const int    q0y = static_cast<int>(std::floor((a_scene.playerY - extent) / 2048.0));
		const int    q1y = static_cast<int>(std::floor((a_scene.playerY + extent) / 2048.0));

		for (int qx = q0x; qx <= q1x; ++qx) {
			for (int qy = q0y; qy <= q1y; ++qy) {
				const double ox = qx * 2048.0;
				const double oy = qy * 2048.0;

				std::vector<Clip4> corners;
				for (int a = 0; a < 2; ++a) {
					for (int b = 0; b < 2; ++b) {
						for (int z = -900; z <= 900; z += 1800) {
							corners.push_back(Clip(a_cullViewProj,
								ox + a * 2048.0 - a_scene.posAdjust[0],
								oy + b * 2048.0 - a_scene.posAdjust[1],
								z - a_scene.posAdjust[2]));
						}
					}
				}
				const double dx = std::max(0.0, std::fabs(ox + 1024.0 - cx) - 7936.0);
				const double dy = std::max(0.0, std::fabs(oy + 1024.0 - cy) - 7936.0);
				if (OutsideSame(corners) || dx * dx + dy * dy > 1704.0 * 1704.0) {
					continue;
				}

				Scene::DrawRange range{ static_cast<UINT>(a_scene.verts.size() / 3), 0, {} };
				range.offset[0] = static_cast<float>(ox) - a_scene.posAdjust[0];
				range.offset[1] = static_cast<float>(oy) - a_scene.posAdjust[1];
				range.offset[2] = 0.0f - a_scene.posAdjust[2];

				const int gi = static_cast<int>(std::lround(ox / spacing));
				const int gj = static_cast<int>(std::lround(oy / spacing));
				for (int i = 0; i < 16; ++i) {
					for (int j = 0; j < 16; ++j) {
						const GridPoint v00{ gi + i, gj + j };
						const GridPoint v10{ gi + i + 1, gj + j };
						const GridPoint v01{ gi + i, gj + j + 1 };
						const GridPoint v11{ gi + i + 1, gj + j + 1 };
						for (const auto& tri : { std::array{ v00, v10, v11 }, std::array{ v00, v11, v01 } }) {
							a_scene.ids.push_back(tri);
							for (const auto& v : tri) {
								a_scene.verts.push_back(static_cast<float>((v.first - gi) * spacing));
								a_scene.verts.push_back(static_cast<float>((v.second - gj) * spacing));
								a_scene.verts.push_back(a_scene.hills ?
										static_cast<float>(Hill(v.first * spacing, v.second * spacing)) :
										0.0f);
							}
							range.count += 3;
						}
					}
				}
				a_scene.draws.push_back(range);
			}
		}
	}

	struct Result
	{
		uint64_t ds{};
		uint64_t generated{};
		uint64_t clipped{};

		std::vector<std::array<float, 4>> factors;
		std::vector<float>                depth;
	};

	struct Textures
	{
		ID3D11ShaderResourceView* field{ nullptr };
		ID3D11ShaderResourceView* ones{ nullptr };
		ID3D11SamplerState*       sampler{ nullptr };
	};

	Textures MakeTextures()
	{
		Textures textures;

		// Torture field: craters -120u (radius 5 texels) with +60u rims on a 16-texel lattice.
		const int          size = 256;
		std::vector<float> field(size * size);
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				const double dx = (x % 16) - 8.0;
				const double dy = (y % 16) - 8.0;
				const double d = std::sqrt(dx * dx + dy * dy);
				double       h = 0.0;
				if (d < 5.0) {
					h = -120.0 * (1.0 - (d / 5.0) * (d / 5.0));
				} else if (d < 7.5) {
					h = 60.0 * std::sin(3.14159265 * (d - 5.0) / 2.5);
				}
				field[y * size + x] = static_cast<float>(h);
			}
		}
		textures.field = MakeTexture(DXGI_FORMAT_R32_FLOAT, size, field.data(), size * 4);

		const std::vector<float> ones(16 * 16, 1.0f);
		textures.ones = MakeTexture(DXGI_FORMAT_R32_FLOAT, 16, ones.data(), 16 * 4);

		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		g_device->CreateSamplerState(&sampler, &textures.sampler);
		return textures;
	}

	struct Shaders
	{
		ID3D11HullShader*   hs{ nullptr };
		ID3D11DomainShader* ds{ nullptr };
		ID3D11VertexShader* vs{ nullptr };
		ID3D11InputLayout*  layout{ nullptr };
	};

	// a_depth false: factor readback through the UAV. True: depth image at a_height.
	Result Run(const Scene& a_scene, const Mat& a_viewProj, float a_screenX,
		const std::vector<uint32_t>& a_activity, const Shaders& a_shaders, float a_height,
		const Textures& a_textures, bool a_depth)
	{
		Result     result;
		const UINT patches = static_cast<UINT>(a_scene.ids.size());
		const UINT width = static_cast<UINT>(std::lround(a_height * 16.0 / 9.0));
		const UINT height = static_cast<UINT>(a_height);

		// CB12 as the generated shaders declare it: c8 view-projection, c32 its inverse,
		// c40 CameraPosAdjust.
		float frameData[44][4]{};
		Mat   inverse{};
		Invert(a_viewProj, inverse);
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) {
				frameData[8 + r][c] = static_cast<float>(a_viewProj.m[r][c]);
				frameData[32 + r][c] = static_cast<float>(inverse.m[r][c]);
			}
		}
		for (int i = 0; i < 3; ++i) {
			frameData[40][i] = a_scene.posAdjust[i];
		}
		ID3D11Buffer* frame = MakeConstants(frameData, sizeof(frameData));

		// Same layout as WindowCB in Clipmap.cpp: Window, Raise, Window1, Screen.
		float windowData[4][4]{};
		windowData[0][0] = static_cast<float>(std::floor(a_scene.playerX / 0.75) * 0.75);
		windowData[0][1] = static_cast<float>(std::floor(a_scene.playerY / 0.75) * 0.75);
		windowData[0][2] = 1022 * 0.75f * 0.80f;
		windowData[0][3] = 1022 * 0.75f * 0.97f;
		windowData[1][0] = 1.0f;
		windowData[2][0] = static_cast<float>(std::floor(a_scene.playerX / 3.0) * 3.0);
		windowData[2][1] = static_cast<float>(std::floor(a_scene.playerY / 3.0) * 3.0);
		windowData[2][2] = 1022 * 3.0f * 0.80f;
		windowData[2][3] = 1022 * 3.0f * 0.97f;
		windowData[3][0] = a_screenX;
		ID3D11Buffer* window = MakeConstants(windowData, sizeof(windowData));

		const float   zero[4]{};
		ID3D11Buffer* drawOffset = MakeConstants(zero, 16);
		ID3D11Buffer* drawBase = MakeConstants(zero, 16);

		ID3D11ShaderResourceView* activity = MakeTexture(DXGI_FORMAT_R32_UINT,
			Clipmap::kActivityTexels, a_activity.data(), Clipmap::kActivityTexels * 4);

		D3D11_BUFFER_DESC vertexDesc{};
		vertexDesc.ByteWidth = static_cast<UINT>(a_scene.verts.size() * 4);
		vertexDesc.Usage = D3D11_USAGE_DEFAULT;
		vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vertexData{ a_scene.verts.data() };
		ID3D11Buffer* vertices = nullptr;
		g_device->CreateBuffer(&vertexDesc, &vertexData, &vertices);

		ID3D11Buffer*              factors = nullptr;
		ID3D11UnorderedAccessView* factorsUAV = nullptr;
		ID3D11Buffer*              factorsRead = nullptr;
		ID3D11Texture2D*           depthTexture = nullptr;
		ID3D11DepthStencilView*    depthView = nullptr;
		ID3D11Texture2D*           depthRead = nullptr;
		ID3D11DepthStencilState*   depthState = nullptr;
		ID3D11RasterizerState*     rasterizer = nullptr;

		if (!a_depth) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = patches * 16;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = 16;
			const std::vector<float> sentinel(patches * 4, -1.0f);
			D3D11_SUBRESOURCE_DATA   initial{ sentinel.data() };
			g_device->CreateBuffer(&desc, &initial, &factors);
			g_device->CreateUnorderedAccessView(factors, nullptr, &factorsUAV);

			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			g_device->CreateBuffer(&desc, nullptr, &factorsRead);

			g_context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1,
				&factorsUAV, nullptr);
		} else {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_D32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			g_device->CreateTexture2D(&desc, nullptr, &depthTexture);
			g_device->CreateDepthStencilView(depthTexture, nullptr, &depthView);

			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			g_device->CreateTexture2D(&desc, nullptr, &depthRead);

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = TRUE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
			g_device->CreateDepthStencilState(&depthDesc, &depthState);

			D3D11_RASTERIZER_DESC rasterDesc{};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.DepthClipEnable = TRUE;
			g_device->CreateRasterizerState(&rasterDesc, &rasterizer);

			g_context->OMSetRenderTargets(0, nullptr, depthView);
			g_context->OMSetDepthStencilState(depthState, 0);
			g_context->RSSetState(rasterizer);
			g_context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 1.0f, 0);
		}

		const UINT stride = 12;
		const UINT offset = 0;
		g_context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
		g_context->IASetInputLayout(a_shaders.layout);
		g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
		g_context->VSSetShader(a_shaders.vs, nullptr, 0);
		g_context->HSSetShader(a_shaders.hs, nullptr, 0);
		g_context->DSSetShader(a_shaders.ds, nullptr, 0);
		g_context->PSSetShader(nullptr, nullptr, 0);
		g_context->VSSetConstantBuffers(12, 1, &frame);
		g_context->VSSetConstantBuffers(2, 1, &drawOffset);
		g_context->HSSetConstantBuffers(12, 1, &frame);
		g_context->DSSetConstantBuffers(12, 1, &frame);
		g_context->DSSetConstantBuffers(3, 1, &drawBase);
		g_context->HSSetConstantBuffers(Clipmap::kParamsSlot, 1, &window);
		g_context->DSSetConstantBuffers(Clipmap::kParamsSlot, 1, &window);
		g_context->HSSetShaderResources(Clipmap::kActivitySlot, 1, &activity);

		// t0 level-0 field, t1 snow coverage (1 = snowy everywhere), t2 level-1 field,
		// t3 mesh cap (1 = no cap).
		ID3D11ShaderResourceView* domainViews[4] = { a_textures.field, a_textures.ones,
			a_textures.field, a_textures.ones };
		g_context->DSSetShaderResources(0, 4, domainViews);
		ID3D11SamplerState* samplers[3] = { a_textures.sampler, a_textures.sampler,
			a_textures.sampler };
		g_context->DSSetSamplers(0, 3, samplers);

		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width),
			static_cast<float>(height), 0.0f, 1.0f };
		g_context->RSSetViewports(1, &viewport);

		D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
		ID3D11Query*     query = nullptr;
		g_device->CreateQuery(&queryDesc, &query);
		g_context->Begin(query);
		for (const auto& draw : a_scene.draws) {
			g_context->UpdateSubresource(drawOffset, 0, nullptr, draw.offset, 0, 0);
			const uint32_t base[4] = { draw.first / 3, 0, 0, 0 };
			g_context->UpdateSubresource(drawBase, 0, nullptr, base, 0, 0);
			g_context->Draw(draw.count, draw.first);
		}
		g_context->End(query);

		D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
		while (g_context->GetData(query, &stats, sizeof(stats), 0) != S_OK) {}
		result.ds = stats.DSInvocations;
		result.generated = stats.CInvocations;
		result.clipped = stats.CPrimitives;

		if (!a_depth) {
			g_context->CopyResource(factorsRead, factors);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			g_context->Map(factorsRead, 0, D3D11_MAP_READ, 0, &mapped);
			result.factors.resize(patches);
			std::memcpy(result.factors.data(), mapped.pData, patches * 16);
			g_context->Unmap(factorsRead, 0);
		} else {
			g_context->CopyResource(depthRead, depthTexture);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			g_context->Map(depthRead, 0, D3D11_MAP_READ, 0, &mapped);
			result.depth.resize(static_cast<size_t>(width) * height);
			for (UINT y = 0; y < height; ++y) {
				std::memcpy(&result.depth[static_cast<size_t>(y) * width],
					static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
					width * 4);
			}
			g_context->Unmap(depthRead, 0);
		}

		g_context->OMSetRenderTargets(0, nullptr, nullptr);
		g_context->RSSetState(nullptr);
		g_context->OMSetDepthStencilState(nullptr, 0);

		for (IUnknown* object : std::initializer_list<IUnknown*>{ frame, window, drawOffset,
				 drawBase, activity, vertices, query, factors, factorsUAV, factorsRead,
				 depthTexture, depthView, depthRead, depthState, rasterizer }) {
			if (object) {
				object->Release();
			}
		}
		return result;
	}

	// Shared-edge integer factor mismatches among drawn patches (culled ones carry -1).
	int CheckCracks(const Scene& a_scene, const Result& a_result)
	{
		std::map<std::array<int, 4>, int> seen;
		int                               bad = 0;
		for (size_t p = 0; p < a_scene.ids.size(); ++p) {
			if (a_result.factors[p][3] < 0.0f) {
				continue;
			}
			const auto&     t = a_scene.ids[p];
			const GridPoint edges[3][2] = { { t[1], t[2] }, { t[2], t[0] }, { t[0], t[1] } };
			for (int k = 0; k < 3; ++k) {
				auto a = edges[k][0];
				auto b = edges[k][1];
				if (b < a) {
					std::swap(a, b);
				}
				const std::array<int, 4> key{ a.first, a.second, b.first, b.second };
				const int                factor = static_cast<int>(std::ceil(a_result.factors[p][k]));
				const auto [it, fresh] = seen.emplace(key, factor);
				if (!fresh && it->second != factor) {
					++bad;
				}
			}
		}
		return bad;
	}

	bool g_detail = false;

	// Integer factor changes between two renders of the same patches.
	int Flips(const Result& a_a, const Result& a_b)
	{
		int flips = 0;
		for (size_t p = 0; p < a_a.factors.size(); ++p) {
			if (a_a.factors[p][3] < 0.0f || a_b.factors[p][3] < 0.0f) {
				continue;
			}
			for (int k = 0; k < 4; ++k) {
				if (std::ceil(a_a.factors[p][k]) != std::ceil(a_b.factors[p][k])) {
					++flips;
					if (g_detail) {
						std::printf("      flip patch %zu k %d: %.7f -> %.7f\n", p, k,
							a_a.factors[p][k], a_b.factors[p][k]);
					}
				}
			}
		}
		return flips;
	}

	int Culled(const Result& a_result)
	{
		int culled = 0;
		for (const auto& f : a_result.factors) {
			if (f[3] < 0.0f) {
				++culled;
			}
		}
		return culled;
	}

	struct Variant
	{
		const char* name;
		bool        cull;
		bool        cap;
		float       snap;
		bool        canonical;

		ID3D11HullShader*   hs{ nullptr };
		ID3D11DomainShader* factorDS{ nullptr };
		ID3D11DomainShader* realDS{ nullptr };
	};

	void Build(Variant& a_variant, const Reflection::Signature& a_signature)
	{
		Settings::tessellationFrustumCull = a_variant.cull;
		Settings::tessellationScreenCap = a_variant.cap;
		Settings::tessellationFactorSnap = a_variant.snap;
		Settings::tessellationCanonicalEdges = a_variant.canonical;

		const std::string common = Tessellation::EmitStruct(a_signature) +
		                           Tessellation::EmitPrologue(true, Tessellation::Mode::kLandscape) +
		                           Tessellation::EmitPatchConstants(true);

		ID3DBlob* hs = Compile(common + Tessellation::EmitHull(Settings::tessellationWinding),
			"hs_5_0");
		ID3DBlob* factorDS = Compile(common +
				"RWStructuredBuffer<float4> Factors : register(u0);\n"
				"cbuffer DrawBase : register(b3) { uint PrimBase; };\n"
				"[domain(\"tri\")]\n"
				"float4 main(PatchConstants pc, float3 bary : SV_DomainLocation,\n"
				"            const OutputPatch<CP, 3> patch, uint prim : SV_PrimitiveID) : SV_POSITION\n"
				"{\n"
				"\tFactors[PrimBase + prim] = float4(pc.edges[0], pc.edges[1], pc.edges[2], pc.inside);\n"
				"\treturn patch[0].f0 * bary.x + patch[1].f0 * bary.y + patch[2].f0 * bary.z;\n"
				"}\n",
			"ds_5_0");
		ID3DBlob* realDS = Compile(common +
				Tessellation::EmitDomain(a_signature, true, Tessellation::Mode::kLandscape),
			"ds_5_0");

		for (IUnknown* object : std::initializer_list<IUnknown*>{ a_variant.hs,
				 a_variant.factorDS, a_variant.realDS }) {
			if (object) {
				object->Release();
			}
		}
		g_device->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr,
			&a_variant.hs);
		g_device->CreateDomainShader(factorDS->GetBufferPointer(), factorDS->GetBufferSize(),
			nullptr, &a_variant.factorDS);
		g_device->CreateDomainShader(realDS->GetBufferPointer(), realDS->GetBufferSize(),
			nullptr, &a_variant.realDS);
		hs->Release();
		factorDS->Release();
		realDS->Release();
	}
}

int main(int a_argc, char** a_argv)
{
	const bool quick = a_argc > 1 && std::string_view(a_argv[1]) == "--jitter";
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1 };
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1,
			D3D11_SDK_VERSION, &g_device, nullptr, &g_context))) {
		std::puts("WARP 11.1 device unavailable");
		return 2;
	}

	Settings::enableTessellation = true;
	Settings::useClipmap = true;
	Settings::clipmapLevels = 2;
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 35.0f;
	Settings::shelterMeshCap = true;
	Settings::enableTessellationBounds = true;
	Settings::enableSurfaceMaterial = false;
	Settings::recomputeNormals = false;
	Settings::terrainBlendingCompatibility = false;

	Reflection::Signature signature{};
	signature.valid = true;
	{
		Reflection::SignatureElement e{};
		e.semanticName = "SV_POSITION";
		e.semanticIndex = 0;
		e.registerIndex = 0;
		e.hlslType = "float4";
		e.mask = 0x0F;
		e.componentType = 3;
		signature.elements = { e };
	}

	Shaders shaders;
	ID3DBlob* vsCode = Compile(
		"cbuffer PerFrame : register(b12) { row_major float4x4 CameraViewProj : packoffset(c8); };\n"
		"cbuffer PerGeometry : register(b2) { float4 Offset; };\n"
		"float4 main(float3 p : POSITION) : SV_POSITION\n"
		"{\n"
		"\treturn mul(CameraViewProj, float4(p + Offset.xyz, 1.0f));\n"
		"}\n",
		"vs_5_0");
	g_device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr,
		&shaders.vs);
	const D3D11_INPUT_ELEMENT_DESC input{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		D3D11_INPUT_PER_VERTEX_DATA, 0 };
	g_device->CreateInputLayout(&input, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
		&shaders.layout);
	vsCode->Release();

	const Textures textures = MakeTextures();

	// "all on" includes the screen cap, which ships off; "shipped" is the INI default.
	Variant variants[] = {
		{ "legacy", false, false, 0.0f, false },
		{ "all on", true, true, 0.015625f, true },
		{ "all on, cull off", false, true, 0.015625f, true },
		{ "cull only", true, false, 0.0f, false },
		{ "snap+canon only", false, false, 0.015625f, true },
		{ "shipped", true, false, 0.015625f, true },
	};
	enum
	{
		kLegacy,
		kAllOn,
		kNoCull,
		kCullOnly,
		kSnapOnly,
		kShipped,
		kVariants
	};

	// A trail of active 48u cells behind the player, so the bounded factors (EdgeBound) and
	// their activity-cell edges are part of the check.
	Scene scene;
	std::set<GridPoint> trail;
	const double cell = Clipmap::CellSizeFor(0) * static_cast<double>(Clipmap::kActivityRatio);
	for (double y = scene.playerY - 760.0; y <= scene.playerY; y += 24.0) {
		const int ix = static_cast<int>(std::floor(scene.playerX / cell));
		const int iy = static_cast<int>(std::floor(y / cell));
		for (int dx = -1; dx <= 1; ++dx) {
			for (int dy = -1; dy <= 1; ++dy) {
				trail.insert({ ix + dx, iy + dy });
			}
		}
	}
	const uint32_t mask = Clipmap::kActivityTexels - 1;
	const float    one = 1.0f;
	uint32_t       oneBits;
	std::memcpy(&oneBits, &one, 4);
	std::vector<uint32_t> activityTrail(Clipmap::kActivityTexels * Clipmap::kActivityTexels, 0);
	std::vector<uint32_t> activityFull(activityTrail.size(), oneBits);
	for (const auto& [x, y] : trail) {
		activityTrail[(y & mask) * Clipmap::kActivityTexels + (x & mask)] = oneBits;
	}

	struct Config
	{
		const char* name;
		float       maxFactor;
		float       targetSpacing;
		float       blanketSpacing;
	};
	const Config configs[] = {
		{ "shipped 64/2/8", 64.0f, 2.0f, 8.0f },
		{ "user 16/8/8", 16.0f, 8.0f, 8.0f },
	};

	struct View
	{
		const char*                  name;
		double                       dy, z, yaw, pitch;
		bool                         hills;
		const std::vector<uint32_t>* activity;
	};
	const View views[] = {
		{ "3rd person, look back at trail", +250.0, 140.0, 180.0, -14.0, false, &activityTrail },
		{ "3rd person, look forward", -250.0, 140.0, 0.0, -10.0, false, &activityTrail },
		{ "saturated window, look back", +250.0, 140.0, 180.0, -14.0, false, &activityFull },
		{ "steep look down at the feet", +120.0, 220.0, 180.0, -55.0, false, &activityTrail },
		{ "1st person, look back", 0.0, 120.0, 180.0, -4.0, false, &activityTrail },
		{ "hills, look back at trail", +250.0, 140.0, 180.0, -14.0, true, &activityTrail },
	};

	const float  heights[] = { 800.0f, 1080.0f, 1440.0f, 2160.0f };
	const double halton[8][2] = { { 0.5, 0.333 }, { 0.25, 0.667 }, { 0.75, 0.111 },
		{ 0.125, 0.444 }, { 0.625, 0.778 }, { 0.375, 0.222 }, { 0.875, 0.556 },
		{ 0.0625, 0.889 } };
	const double aspect = 16.0 / 9.0;
	const double P11 = 1.2071 * aspect;
	const float  pixels = 4.0f;

	long long raised = 0;
	long long cracks[kVariants]{};
	long long flips[kVariants]{};
	long long flipsHills[kVariants]{};
	long long depthDiff = 0;
	long long depthRuns = 0;
	long long clipDiff = 0;

	for (const auto& config : configs) {
		Settings::tessellationMaxFactor = config.maxFactor;
		Settings::tessellationTargetSpacing = config.targetSpacing;
		Settings::tessellationBlanketSpacing = config.blanketSpacing;
		for (auto& variant : variants) {
			Build(variant, signature);
		}

		for (const auto& view : views) {
			scene.hills = view.hills;
			const double ground = view.hills ? Hill(scene.playerX, scene.playerY + view.dy) : 0.0;
			const double camX = scene.playerX + 0.3719;
			const double camY = scene.playerY + view.dy + 0.6180;
			const double camZ = ground + view.z + 0.2113;
			scene.posAdjust[0] = static_cast<float>(camX);
			scene.posAdjust[1] = static_cast<float>(camY);
			scene.posAdjust[2] = static_cast<float>(camZ);
			const Camera camera = MakeCamera(camX - scene.posAdjust[0], camY - scene.posAdjust[1],
				camZ - scene.posAdjust[2], view.yaw, view.pitch, P11, aspect);
			const Mat viewProj = ViewProj(camera, 0.0, 0.0);
			BuildScene(scene, viewProj);

			for (const float height : heights) {
				if (quick && height != 1080.0f) {
					continue;
				}
				const float screenX = height * 0.5f / pixels;

				const auto factorShaders = [&](int a_variant) {
					return Shaders{ variants[a_variant].hs, variants[a_variant].factorDS,
						shaders.vs, shaders.layout };
				};

				Result r[kVariants];
				for (int v = 0; v < kVariants; ++v) {
					r[v] = Run(scene, viewProj, variants[v].cap ? screenX : 0.0f, *view.activity,
						factorShaders(v), height, textures, false);
				}
				for (int v = 1; v < kVariants; ++v) {
					for (size_t p = 0; p < r[v].factors.size(); ++p) {
						if (r[v].factors[p][3] < 0.0f) {
							continue;
						}
						for (int k = 0; k < 4; ++k) {
							if (std::ceil(r[v].factors[p][k]) > std::ceil(r[kLegacy].factors[p][k])) {
								++raised;
							}
						}
					}
				}

				const auto saved = [](uint64_t a_now, uint64_t a_before) {
					return a_before ? 100.0 * (1.0 - static_cast<double>(a_now) / static_cast<double>(a_before)) : 0.0;
				};
				std::printf("%-14s | %-30s | H=%4.0f | patches %5zu | legacy DS %8llu gen %8llu clip %7llu || "
							"all on DS %8llu (-%4.1f%%) gen %8llu (-%4.1f%%) clip %7llu (-%4.1f%%) culled %5d || "
							"DS: shipped -%4.1f%% | cull only -%4.1f%% | cap+snap only -%4.1f%% | snap only -%4.1f%%\n",
					config.name, view.name, height, scene.ids.size(), r[kLegacy].ds,
					r[kLegacy].generated, r[kLegacy].clipped, r[kAllOn].ds,
					saved(r[kAllOn].ds, r[kLegacy].ds), r[kAllOn].generated,
					saved(r[kAllOn].generated, r[kLegacy].generated), r[kAllOn].clipped,
					saved(r[kAllOn].clipped, r[kLegacy].clipped), Culled(r[kAllOn]),
					saved(r[kShipped].ds, r[kLegacy].ds), saved(r[kCullOnly].ds, r[kLegacy].ds),
					saved(r[kNoCull].ds, r[kLegacy].ds), saved(r[kSnapOnly].ds, r[kLegacy].ds));
				if (r[kCullOnly].clipped != r[kLegacy].clipped ||
					r[kShipped].clipped != r[kSnapOnly].clipped) {
					++clipDiff;
				}

				if (height != 1080.0f) {
					continue;
				}

				for (int v = 0; v < kVariants; ++v) {
					cracks[v] += CheckCracks(scene, r[v]);
				}
				for (int phase = 0; phase < 8; ++phase) {
					const double jx = (halton[phase][0] - 0.5) * 2.0 / (height * aspect);
					const double jy = (halton[phase][1] - 0.5) * 2.0 / height;
					const Mat    jittered = ViewProj(camera, jx, jy);
					for (int v = 0; v < kVariants; ++v) {
						g_detail = v == kAllOn;
						const Result rj = Run(scene, jittered, variants[v].cap ? screenX : 0.0f,
							*view.activity, factorShaders(v), height, textures, false);
						(view.hills ? flipsHills[v] : flips[v]) += Flips(r[v], rj);
						cracks[v] += CheckCracks(scene, rj);
					}
					g_detail = false;

					// B) real displaced depth: cull on and cull off must be bit-identical, with the
					// screen cap on and with the shipped settings.
					if (quick || phase % 3 != 0) {
						continue;
					}
					const std::pair<int, int> pairs[] = { { kAllOn, kNoCull }, { kShipped, kSnapOnly } };
					for (const auto& [cullOn, cullOff] : pairs) {
						const Shaders on{ variants[cullOn].hs, variants[cullOn].realDS, shaders.vs,
							shaders.layout };
						const Shaders off{ variants[cullOff].hs, variants[cullOff].realDS, shaders.vs,
							shaders.layout };
						const float  screen = variants[cullOn].cap ? screenX : 0.0f;
						const Result depthOn = Run(scene, jittered, screen, *view.activity, on, height,
							textures, true);
						const Result depthOff = Run(scene, jittered, screen, *view.activity, off, height,
							textures, true);
						long long diff = 0;
						long long covered = 0;
						for (size_t i = 0; i < depthOn.depth.size(); ++i) {
							if (std::memcmp(&depthOn.depth[i], &depthOff.depth[i], 4) != 0) {
								++diff;
							}
							if (depthOff.depth[i] < 1.0f) {
								++covered;
							}
						}
						depthDiff += diff;
						++depthRuns;
						std::printf("    depth %-30s phase %d %-7s: %lld of %lld covered pixels differ "
									"(DS %llu -> %llu, clipped prims %llu vs %llu)\n",
							view.name, phase, variants[cullOn].name, diff, covered, depthOff.ds,
							depthOn.ds, depthOff.clipped, depthOn.clipped);
					}
				}
			}
		}
	}

	std::printf("\nfactors above legacy anywhere (all variants, drawn patches): %lld\n", raised);
	std::printf("views where the cull changed the clipped-primitive count: %lld\n", clipDiff);
	std::printf("depth images cull on vs off: %lld differing pixels over %lld renders\n", depthDiff,
		depthRuns);
	std::printf("1080p, N=%.0f, 9 renders per scene (1 + 8 jitter offsets), per-quadrant draws:\n",
		pixels);
	for (int v = 0; v < kVariants; ++v) {
		std::printf("  %-16s shared-edge factor mismatches: %6lld | factor changes under jitter: "
					"flat views %6lld, hills views %6lld\n",
			variants[v].name, cracks[v], flips[v], flipsHills[v]);
	}

	// Flat ground puts factors exactly on integers, and there the snap must remove every
	// flip. On hills lengths are continuous, so a value can sit within float noise of a
	// step; the savings may not add any flips beyond what the snap-only build keeps.
	const bool ok = raised == 0 && clipDiff == 0 && depthDiff == 0 && cracks[kAllOn] == 0 &&
	                cracks[kNoCull] == 0 && cracks[kCullOnly] == 0 && cracks[kSnapOnly] == 0 &&
	                cracks[kShipped] == 0 && flips[kAllOn] == 0 && flips[kShipped] == 0 &&
	                flipsHills[kAllOn] <= flipsHills[kSnapOnly] &&
	                flipsHills[kShipped] <= flipsHills[kSnapOnly] && (quick || depthRuns > 0);
	std::printf("%s\n", ok ? "HULL CHECK PASS" : "HULL CHECK FAIL");
	return ok ? 0 : 1;
}
