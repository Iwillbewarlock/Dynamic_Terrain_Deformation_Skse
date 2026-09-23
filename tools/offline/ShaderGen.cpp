// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ClipmapUpdateCS.h"
#include "StampShapeAnalysisCS.h"
#include "ShaderReflection.cpp"
#include "Tessellation.cpp"

namespace ShaderRegistry
{
	Bytecode For(ID3D11VertexShader*) { return {}; }
}
#include "UtilityRouting.h"

#include <filesystem>
#include <fstream>

namespace
{
	Reflection::SignatureElement Element(
		const char* a_semantic, uint32_t a_index, uint32_t a_reg, const char* a_type)
	{
		Reflection::SignatureElement e{};
		e.semanticName = a_semantic;
		e.semanticIndex = a_index;
		e.registerIndex = a_reg;
		e.hlslType = a_type;
		e.mask = std::string_view(a_type) == "float4" ? 0x0F :
		         std::string_view(a_type) == "float3" ? 0x07 :
		         std::string_view(a_type) == "float2" ? 0x03 :
		                                                0x01;
		e.componentType = 3;
		return e;
	}

	Reflection::Signature ColourSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, "float4"),
			Element("TEXCOORD", 0, 1, "float4"),
			Element("TEXCOORD", 4, 2, "float3"),
			Element("TEXCOORD", 1, 3, "float3"),
			Element("TEXCOORD", 2, 4, "float3"),
			Element("TEXCOORD", 3, 5, "float3"),
			Element("TEXCOORD", 5, 6, "float3"),
			Element("TEXCOORD", 6, 7, "float4"),
			Element("TEXCOORD", 7, 8, "float4"),
			Element("TEXCOORD", 8, 9, "float3"),
			Element("TEXCOORD", 9, 10, "float3"),
			Element("TEXCOORD", 10, 11, "float3"),
			Element("POSITION", 1, 12, "float4"),
			Element("POSITION", 2, 13, "float4"),
			Element("COLOR", 0, 14, "float4"),
			Element("COLOR", 1, 15, "float4"),
		};
		return s;
	}

	Reflection::Signature DepthPrepassSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = { Element("SV_POSITION", 0, 0, "float4") };
		return s;
	}

	Reflection::Signature ShadowMapSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, "float4"),
			Element("TEXCOORD", 2, 1, "float3"),
		};
		return s;
	}

	Reflection::Signature ActorSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, "float4"),
			Element("TEXCOORD", 0, 1, "float2"),
			Element("TEXCOORD", 4, 2, "float3"),
			Element("TEXCOORD", 1, 3, "float3"),
			Element("TEXCOORD", 2, 4, "float3"),
			Element("TEXCOORD", 3, 5, "float3"),
			Element("TEXCOORD", 5, 6, "float3"),
			Element("TEXCOORD", 8, 7, "float3"),
			Element("TEXCOORD", 9, 8, "float3"),
			Element("TEXCOORD", 10, 9, "float3"),
			Element("POSITION", 1, 10, "float4"),
			Element("POSITION", 2, 11, "float4"),
			Element("COLOR", 0, 12, "float4"),
			Element("COLOR", 1, 13, "float4"),
		};
		return s;
	}

	Reflection::Signature ActorNoNormalSignature()
	{
		Reflection::Signature s = ActorSignature();

		std::erase_if(s.elements, [](const Reflection::SignatureElement& a_e) {
			return a_e.semanticName == "TEXCOORD" && a_e.semanticIndex >= 1 &&
			       a_e.semanticIndex <= 3;
		});

		for (uint32_t i = 0; i < s.elements.size(); ++i) {
			s.elements[i].registerIndex = i;
		}
		return s;
	}

	int g_failures = 0;

	void Check(const std::string& a_source, const char* a_target, const std::string& a_label,
		const std::filesystem::path& a_outDir)
	{
		const auto path = a_outDir / (a_label + ".hlsl");
		std::ofstream(path) << a_source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(a_source.c_str(), a_source.size(), a_label.c_str(),
			nullptr, nullptr, "main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", a_label.c_str(), a_target,
				code->GetBufferSize(), path.string().c_str());
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", a_label.c_str(), a_target,
				static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
	}

	void GenerateCompute(const std::filesystem::path& a_outDir)
	{
		const std::string maxStamps = std::to_string(Clipmap::kMaxStamps);
		const D3D_SHADER_MACRO defines[] = {
			{ "MAX_STAMPS", maxStamps.c_str() },
			{ nullptr, nullptr }
		};

		const std::string source(Clipmap::UpdateShaderSource());
		const auto        path = a_outDir / "clipmap_update.hlsl";
		std::ofstream(path) << source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(source.c_str(), source.size(), "ClipmapUpdateCS",
			defines, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
			&errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", "clipmap_update",
				"cs_5_0", code->GetBufferSize(), path.string().c_str());
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", "clipmap_update",
				"cs_5_0", static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
	}

	void GenerateShapeAnalysis(const std::filesystem::path& a_outDir)
	{
		const std::string source(StampShapes::kAnalysisShader);
		const auto        path = a_outDir / "stamp_shape_analysis.hlsl";
		std::ofstream(path) << source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(source.c_str(), source.size(), "StampShapeAnalysis",
			nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &code,
			&errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", "stamp_shape_analysis",
				"cs_5_0", code->GetBufferSize(), path.string().c_str());
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", "stamp_shape_analysis",
				"cs_5_0", static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
	}

	void Generate(const char* a_name, const Reflection::Signature& a_signature,
		const std::filesystem::path& a_outDir,
		Tessellation::Mode a_mode = Tessellation::Mode::kLandscape)
	{

		const bool displace = Tessellation::WantsDisplacement(a_mode);
		const bool tessellate = Tessellation::WantsSubdivision(a_mode);

		const std::string common = Tessellation::EmitStruct(a_signature) +
		                           Tessellation::EmitPrologue(displace, a_mode) +
		                           Tessellation::EmitPatchConstants(tessellate);

		Check(common + Tessellation::EmitHull(Settings::tessellationWinding), "hs_5_0",
			std::string(a_name) + "_hs", a_outDir);
		Check(common + Tessellation::EmitDomain(a_signature, displace, a_mode), "ds_5_0",
			std::string(a_name) + "_ds", a_outDir);
	}

	// Disassembled patch-constant phases (hs_fork/hs_join) of the hull shader built for a
	// signature. Colour and depth prepass must match text for text, or depth-equal breaks.
	std::string FactorPhase(const Reflection::Signature& a_signature, Tessellation::Mode a_mode)
	{
		const std::string source = Tessellation::EmitStruct(a_signature) +
		                           Tessellation::EmitPrologue(true, a_mode) +
		                           Tessellation::EmitPatchConstants(true) +
		                           Tessellation::EmitHull(Settings::tessellationWinding);

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		std::string phase;
		if (SUCCEEDED(D3DCompile(source.c_str(), source.size(), "factors", nullptr, nullptr,
				"main", "hs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
			ID3DBlob* text = nullptr;
			if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0,
					nullptr, &text))) {
				const std::string all(static_cast<const char*>(text->GetBufferPointer()),
					text->GetBufferSize());
				size_t start = all.find("hs_fork_phase");
				if (start == std::string::npos) {
					start = all.find("hs_join_phase");
				}
				if (start != std::string::npos) {
					phase = all.substr(start);
				}
				text->Release();
			}
		}
		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
		return phase;
	}

	void CheckSameFactors(const std::string& a_label)
	{
		for (const auto mode : { Tessellation::Mode::kLandscape, Tessellation::Mode::kBloodDecal }) {
			const std::string label =
				a_label + (mode == Tessellation::Mode::kLandscape ? " land" : " blood");
			const std::string colour = FactorPhase(ColourSignature(), mode);
			const std::string depth = FactorPhase(DepthPrepassSignature(), mode);
			if (colour.empty() || colour != depth) {
				++g_failures;
				std::printf("  FAIL  %-44s colour and depth patch-constant phases differ\n",
					label.c_str());
			} else {
				std::printf("  PASS  %-44s colour == depth patch-constant phase (%zu chars)\n",
					label.c_str(), colour.size());
			}
		}
	}

	// The generated ClipmapWindow (b13) must keep the layout of WindowCB in Clipmap.cpp:
	// Window c0, Raise c1, Window1 or the WindowReserved pad c2, Screen c3, 64 bytes at most.
	void CheckWindowLayout(const std::string& a_label, const Reflection::Signature& a_signature,
		Tessellation::Mode a_mode)
	{
		const bool displace = Tessellation::WantsDisplacement(a_mode);
		const bool tessellate = Tessellation::WantsSubdivision(a_mode);
		const bool field = a_mode == Tessellation::Mode::kLandscape ||
		                   a_mode == Tessellation::Mode::kBloodDecal;
		const bool screen = field && Settings::useClipmap && Settings::tessellationScreenCap;
		const bool coarse = Clipmap::LevelCount() > 1;

		const std::pair<const char*, int> expected[] = {
			{ "Window", 0 },
			{ "Raise", 16 },
			{ "Window1", coarse ? 32 : -1 },
			{ "WindowReserved", screen && !coarse ? 32 : -1 },
			{ "Screen", screen ? 48 : -1 },
		};

		const std::string common = Tessellation::EmitStruct(a_signature) +
		                           Tessellation::EmitPrologue(displace, a_mode) +
		                           Tessellation::EmitPatchConstants(tessellate);
		const std::pair<std::string, const char*> stages[] = {
			{ common + Tessellation::EmitHull(Settings::tessellationWinding), "hs_5_0" },
			{ common + Tessellation::EmitDomain(a_signature, displace, a_mode), "ds_5_0" },
		};

		int bad = 0;
		int bound = 0;
		for (const auto& [source, target] : stages) {
			ID3DBlob*               code = nullptr;
			ID3DBlob*               errors = nullptr;
			ID3D11ShaderReflection* reflection = nullptr;
			if (FAILED(D3DCompile(source.c_str(), source.size(), "window", nullptr, nullptr,
					"main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
				FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
					IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection)))) {
				++bad;
			} else {
				auto* buffer = reflection->GetConstantBufferByName("ClipmapWindow");
				D3D11_SHADER_BUFFER_DESC desc{};
				if (SUCCEEDED(buffer->GetDesc(&desc))) {
					++bound;
					if (desc.Size > 64) {
						++bad;
					}
					for (const auto& [name, offset] : expected) {
						D3D11_SHADER_VARIABLE_DESC variable{};
						const int at = SUCCEEDED(buffer->GetVariableByName(name)->GetDesc(&variable)) ?
						                   static_cast<int>(variable.StartOffset) :
						                   -1;
						if (at != offset) {
							++bad;
							std::printf("        %s %s at %d, expected %d\n", target, name, at,
								offset);
						}
					}
				}
			}
			for (IUnknown* object : std::initializer_list<IUnknown*>{ code, errors, reflection }) {
				if (object) {
					object->Release();
				}
			}
		}

		// The domain shader always reads the window; the hull shader does when it tessellates.
		if (bad > 0 || bound != (tessellate ? 2 : 1)) {
			++g_failures;
			std::printf("  FAIL  %-44s ClipmapWindow layout (%d stage%s bound)\n",
				a_label.c_str(), bound, bound == 1 ? "" : "s");
		} else {
			std::printf("  PASS  %-44s ClipmapWindow matches WindowCB%s\n", a_label.c_str(),
				screen ? ", Screen at c3" : "");
		}
	}

	void SetSavings(bool a_cull, bool a_cap, float a_snap, bool a_canonical)
	{
		Settings::tessellationFrustumCull = a_cull;
		Settings::tessellationScreenCap = a_cap;
		Settings::tessellationFactorSnap = a_snap;
		Settings::tessellationCanonicalEdges = a_canonical;
	}

	// With every new switch off, nothing the savings add may appear in any generated source.
	void CheckLegacyText()
	{
		const bool  cull = Settings::tessellationFrustumCull;
		const bool  cap = Settings::tessellationScreenCap;
		const bool  canonical = Settings::tessellationCanonicalEdges;
		const float snap = Settings::tessellationFactorSnap;
		const int   tint = Settings::debugTessellationColour;
		SetSavings(false, false, 0.0f, false);
		Settings::debugTessellationColour = 0;

		const char* const added[] = { "kDisplaceBound", "PatchOffScreen", "FrustumSides",
			"ScreenCap", "Screen;", "WindowReserved", "EdgeTess", "kFactorSnap", "placed",
			"pc.inside" };
		int found = 0;
		for (const auto mode : { Tessellation::Mode::kLandscape, Tessellation::Mode::kBloodDecal }) {
			for (const auto& signature : { ColourSignature(), DepthPrepassSignature() }) {
				const bool displace = Tessellation::WantsDisplacement(mode);
				const std::string text =
					Tessellation::EmitPrologue(displace, mode) +
					Tessellation::EmitPatchConstants(Tessellation::WantsSubdivision(mode)) +
					Tessellation::EmitDomain(signature, displace, mode);
				for (const char* word : added) {
					if (text.find(word) != std::string::npos) {
						++found;
						std::printf("  FAIL  all savings switches off still emit \"%s\"\n", word);
					}
				}
			}
		}
		if (found == 0) {
			std::puts("  PASS  all savings switches off emit none of the new code");
		}
		g_failures += found;

		SetSavings(cull, cap, snap, canonical);
		Settings::debugTessellationColour = tint;
	}
}

void CheckAsyncCompiler()
{
    const auto signature = ColourSignature();
    const bool displace = Tessellation::WantsDisplacement(Tessellation::Mode::kLandscape);
    const auto common = Tessellation::EmitStruct(signature) +
        Tessellation::EmitPrologue(displace, Tessellation::Mode::kLandscape) +
        Tessellation::EmitPatchConstants(true);
    ShaderCompiler::Worker worker;
    auto job = std::make_shared<ShaderCompiler::Job>();
    job->hullSource = common + Tessellation::EmitHull(Settings::tessellationWinding);
    job->domainSource = common + Tessellation::EmitDomain(signature, displace, Tessellation::Mode::kLandscape);
    worker.Submit(job);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!job->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!job->ready.load(std::memory_order_acquire) || !job->hull || !job->domain || !job->error.empty()) {
        ++g_failures;
        std::puts("FAIL asynchronous compiler publication");
    } else {
        std::printf("PASS asynchronous compiler publication (%.2f ms off caller thread)\n", job->milliseconds);
    }
    auto invalid = std::make_shared<ShaderCompiler::Job>();
    invalid->hullSource = "invalid shader";
    worker.Submit(invalid);
    const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!invalid->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < failureDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!invalid->ready.load(std::memory_order_acquire) || invalid->error.empty()) {
        ++g_failures;
        std::puts("FAIL asynchronous compile failure reporting");
    } else {
        std::puts("PASS asynchronous compile failure reporting");
    }
    worker.CancelQueued();
}

int main(int a_argc, char** a_argv)
{
	// --legacy: every hull-savings switch off, for a byte-for-byte diff against the old generator.
	std::filesystem::path outDir = "generated_shaders";
	bool legacy = false;
	for (int i = 1; i < a_argc; ++i) {
		if (std::string_view(a_argv[i]) == "--legacy") {
			legacy = true;
		} else {
			outDir = a_argv[i];
		}
	}
	if (legacy) {
		SetSavings(false, false, 0.0f, false);
	}
	std::filesystem::create_directories(outDir);

	Settings::enableTessellation = true;
	Settings::useClipmap = true;
	Settings::recomputeNormals = true;
	Settings::enableSurfaceMaterial = true;

	std::printf("Clipmap update pass\n");
	GenerateCompute(outDir);
	GenerateShapeAnalysis(outDir);

	std::printf("\nDefault configuration (clipmap, normals, surface material)\n");
	Generate("colour", ColourSignature(), outDir);
	Generate("depth_prepass", DepthPrepassSignature(), outDir);
	Settings::terrainBlendingCompatibility = true;
	Generate("tb_depth_correction", DepthPrepassSignature(), outDir);
	Generate("tb_colour_correction", ColourSignature(), outDir);
	Settings::terrainBlendingCompatibility = false;
	Generate("shadow_map", ShadowMapSignature(), outDir);

	std::printf("\nSnow raise (all three passes must build the same displacement)\n");
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 24.0f;
	Generate("raise_colour", ColourSignature(), outDir);
	Generate("raise_depth_prepass", DepthPrepassSignature(), outDir);
	Generate("raise_shadow_map", ShadowMapSignature(), outDir);

	Settings::recomputeNormals = false;
	Generate("raise_nonormals", ColourSignature(), outDir);
	Settings::recomputeNormals = true;

	Settings::enableTessellationBounds = false;
	Generate("blanket_bounds_off", ColourSignature(), outDir);
	Settings::enableTessellationBounds = true;
	Settings::tessellationBlanketSpacing = 0.0f;
	Generate("blanket_floor_off", ColourSignature(), outDir);
	Settings::tessellationBlanketSpacing = 8.0f;
	Generate("blanket_floor_on", ColourSignature(), outDir);
	Generate("blanket_floor_depth", DepthPrepassSignature(), outDir);

	Settings::snowRaiseHeight = 0.0f;
	Generate("raise_zero", ColourSignature(), outDir);
	Settings::enableSnowRaise = false;

	std::printf("\nSurface material off (the proven-vanilla generated source)\n");
	Settings::enableSurfaceMaterial = false;
	Generate("nomaterial", ColourSignature(), outDir);
	Settings::enableSurfaceMaterial = true;

	std::printf("\nRecomputed normals off\n");
	Settings::recomputeNormals = false;
	Generate("noNormals", ColourSignature(), outDir);
	Settings::recomputeNormals = true;

	std::printf("\nAnalytic wave instead of the clipmap\n");
	Settings::useClipmap = false;
	Settings::debugWaveAmplitude = 8.0f;
	Generate("wave", ColourSignature(), outDir);
	Settings::useClipmap = true;
	Settings::debugWaveAmplitude = 0.0f;

	std::printf("\nActor paint (factor 1, no displacement, COLOR0 tint)\n");
	Settings::enableActorPaint = true;
	Settings::debugActorTint = 1.0f;
	Generate("actor_paint", ActorSignature(), outDir, Tessellation::Mode::kActorPaint);

	std::printf("\nActor paint with the tint off (must be a pure pass-through)\n");
	Settings::debugActorTint = 0.0f;
	Generate("actor_plain", ActorSignature(), outDir, Tessellation::Mode::kActorPaint);

	std::printf("\nActor paint on a signature with no TBN\n");
	Generate("actor_notbn", ActorNoNormalSignature(), outDir, Tessellation::Mode::kActorPaint);

	std::printf("\nActor paint mask debug (DebugPaintMask 3)\n");
	Settings::debugPaintMask = 3;
	Generate("actor_maskdebug", ActorSignature(), outDir, Tessellation::Mode::kActorPaint);

	Generate("actor_maskdebug_notbn", ActorNoNormalSignature(), outDir,
		Tessellation::Mode::kActorPaint);

	std::printf("\nActor coat strength debug (DebugPaintMask 4)\n");
	Settings::debugPaintMask = 4;
	Generate("actor_coatdebug", ActorSignature(), outDir, Tessellation::Mode::kActorPaint);
	Settings::debugPaintMask = 0;

	Settings::enableActorPaint = false;

	std::printf("\nField debug: ramp, bands, and the blend-weight fallback\n");
	for (int mode = 1; mode <= 3; ++mode) {
		Settings::debugFieldColour = mode;
		const std::string name = "fielddebug_" + std::to_string(mode);
		Generate(name.c_str(), ColourSignature(), outDir);
	}
	Settings::debugFieldColour = 0;

	std::printf("\nTrap 8 probe (displaces at factor 1, reads nothing)\n");
	Settings::enableStaticProbe = true;
	Settings::staticProbeOffset = 30.0f;
	Generate("probe_lit", ColourSignature(), outDir, Tessellation::Mode::kStaticProbe);
	Generate("probe_depth", DepthPrepassSignature(), outDir,
		Tessellation::Mode::kStaticProbe);
	Generate("probe_shadow", ShadowMapSignature(), outDir,
		Tessellation::Mode::kStaticProbe);

	Settings::staticProbeOffset = 0.0f;
	Generate("probe_zero", ColourSignature(), outDir, Tessellation::Mode::kStaticProbe);

	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 24.0f;
	Settings::staticProbeOffset = 30.0f;
	Generate("probe_with_raise", ColourSignature(), outDir,
		Tessellation::Mode::kStaticProbe);
	Settings::enableSnowRaise = false;
	Settings::snowRaiseHeight = 0.0f;
	Settings::staticProbeOffset = 0.0f;
	Settings::enableStaticProbe = false;

	std::printf("\nMesh raise (band below the bound, factor 1)\n");
	Settings::enableMeshRaise = true;
	Settings::meshRaiseHeight = 32.0f;
	Generate("mesh_lit", ColourSignature(), outDir, Tessellation::Mode::kMeshRaise);
	Generate("mesh_depth", DepthPrepassSignature(), outDir,
		Tessellation::Mode::kMeshRaise);
	Generate("mesh_shadow", ShadowMapSignature(), outDir,
		Tessellation::Mode::kMeshRaise);

	Settings::enableSurfaceMaterial = false;
	Generate("mesh_nomaterial", ColourSignature(), outDir,
		Tessellation::Mode::kMeshRaise);
	Settings::enableSurfaceMaterial = true;

	Settings::meshRaiseHeight = 0.0f;
	Generate("mesh_zero", ColourSignature(), outDir, Tessellation::Mode::kMeshRaise);
	Settings::enableMeshRaise = false;

	std::printf("\nCounter-clockwise winding\n");
	Settings::tessellationWinding = "ccw";
	Generate("ccw", ColourSignature(), outDir);

	if (!legacy) {
		std::printf("\nHull savings: frustum cull, screen cap, factor snap, canonical edges\n");
		// Every switch on, the screen cap included (it ships off); the defaults come back after.
		const bool  defaultCull = Settings::tessellationFrustumCull;
		const bool  defaultCap = Settings::tessellationScreenCap;
		const float defaultSnap = Settings::tessellationFactorSnap;
		const bool  defaultCanonical = Settings::tessellationCanonicalEdges;
		SetSavings(true, true, 0.015625f, true);
		Settings::enableSnowRaise = true;
		Settings::snowRaiseHeight = 35.0f;
		Generate("savings_colour", ColourSignature(), outDir);
		Generate("savings_depth", DepthPrepassSignature(), outDir);
		Generate("savings_blood", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
		Settings::clipmapLevels = 1;
		Generate("savings_one_level_colour", ColourSignature(), outDir);
		Generate("savings_one_level_depth", DepthPrepassSignature(), outDir);
		Settings::clipmapLevels = 2;
		Settings::enableTessellationBounds = false;
		Generate("savings_unbounded", ColourSignature(), outDir);
		Settings::enableTessellationBounds = true;
		Settings::debugWorldZOffset = 40.0f;
		Generate("savings_zoffset", ColourSignature(), outDir);
		Settings::debugWorldZOffset = 0.0f;
		Settings::debugTessellationColour = 1;
		Generate("tessdebug_colour", ColourSignature(), outDir);
		Generate("tessdebug_depth", DepthPrepassSignature(), outDir);
		Settings::debugTessellationColour = 0;

		SetSavings(true, false, 0.0f, false);
		Generate("cull_only_colour", ColourSignature(), outDir);
		Generate("cull_only_depth", DepthPrepassSignature(), outDir);
		SetSavings(false, true, 0.0f, false);
		Generate("cap_only_colour", ColourSignature(), outDir);
		Settings::clipmapLevels = 1;
		Generate("cap_only_one_level", DepthPrepassSignature(), outDir);
		Settings::clipmapLevels = 2;
		SetSavings(false, false, 0.015625f, false);
		Generate("snap_only_colour", ColourSignature(), outDir);
		SetSavings(false, false, 0.0f, true);
		Generate("canonical_only_colour", ColourSignature(), outDir);
		SetSavings(false, false, 0.0f, false);
		Generate("savings_off_colour", ColourSignature(), outDir);
		Generate("savings_off_depth", DepthPrepassSignature(), outDir);
		SetSavings(true, true, 0.015625f, true);

		Settings::useClipmap = false;
		Settings::debugWaveAmplitude = 8.0f;
		Generate("savings_wave", ColourSignature(), outDir);
		Settings::useClipmap = true;
		Settings::debugWaveAmplitude = 0.0f;

		std::printf("\nClipmapWindow (b13) as declared by every shader that binds it\n");
		Settings::enableMeshRaise = true;
		Settings::meshRaiseHeight = 32.0f;
		for (const uint32_t levels : { 1u, 2u }) {
			for (const bool cap : { false, true }) {
				Settings::clipmapLevels = levels;
				Settings::tessellationScreenCap = cap;
				const std::string suffix = std::format(" levels={} cap={}", levels, cap);
				CheckWindowLayout("window land colour" + suffix, ColourSignature(),
					Tessellation::Mode::kLandscape);
				CheckWindowLayout("window land depth" + suffix, DepthPrepassSignature(),
					Tessellation::Mode::kLandscape);
				CheckWindowLayout("window blood colour" + suffix, ColourSignature(),
					Tessellation::Mode::kBloodDecal);
				CheckWindowLayout("window blood depth" + suffix, DepthPrepassSignature(),
					Tessellation::Mode::kBloodDecal);
				CheckWindowLayout("window mesh raise" + suffix, ColourSignature(),
					Tessellation::Mode::kMeshRaise);
			}
		}
		Settings::enableMeshRaise = false;
		Settings::meshRaiseHeight = 0.0f;
		Settings::clipmapLevels = 2;
		Settings::tessellationScreenCap = true;

		std::printf("\nColour and depth prepass must build bit-identical factors\n");
		// off, shipped (every switch but the screen cap), and all on.
		for (const std::string_view savings : { "off", "shipped", "on" }) {
			const bool on = savings != "off";
			for (const bool blend : { false, true }) {
				for (const uint32_t levels : { 1u, 2u }) {
					SetSavings(on, savings == "on", on ? 0.015625f : 0.0f, on);
					Settings::terrainBlendingCompatibility = blend;
					Settings::clipmapLevels = levels;
					CheckSameFactors(std::format("factors savings={} tb={} levels={}", savings,
						blend ? 1 : 0, levels));
				}
			}
		}
		SetSavings(defaultCull, defaultCap, defaultSnap, defaultCanonical);
		Settings::terrainBlendingCompatibility = false;
		Settings::clipmapLevels = 2;

		Settings::enableSnowRaise = false;
		Settings::snowRaiseHeight = 0.0f;
	}

	CheckLegacyText();

	CheckAsyncCompiler();

	Settings::useClipmap = true;
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 35.0f;
	Settings::shelterMeshCap = true;
	Settings::enableTessellationBounds = true;
	Generate("blood_colour", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Generate("blood_depth", DepthPrepassSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::enableTessellationBounds = false;
	Generate("blood_unbounded", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::enableSnowRaise = false;
	Generate("blood_no_raise", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::useClipmap = false;
	Generate("blood_no_field", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);

	for (const auto& [technique, expected] : std::initializer_list<std::pair<uint32_t, bool>>{
		{ 0x2000u, true }, { 0x2081u, true },
		{ 0xC000u, false }, { 0x14000u, false },
		{ 0xE000u, false },
		{ 0x1200u, false }, { 0x3200u, false },
		{ 0x202000u, false }, { 0x402000u, false },
		{ 0x802000u, false }, { 0x1002000u, false },
		{ 0x10002000u, false }, { 0u, false } }) {
		if (UtilityRouting::IsCameraDepth(technique) != expected) {
			std::printf("FAIL: Utility routing %08X\n", technique);
			++g_failures;
		}
	}

	{

		const float orthographic[16] = {
			0.0004f, 0.0001f, 0.0000f, 0.0000f,
			0.0000f, 0.0003f, -0.0000f, 0.0000f,
			0.0001f, -0.0002f, -0.0000f, 0.0000f,
			-1.0000f, 1.0000f, -0.0001f, 1.0000f
		};
		const float perspective[16] = {
			1.2071f, 0.0000f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 1.0000f, 1.0000f,
			0.0000f, 2.1445f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 14.7349f, 0.0000f
		};

		if (UtilityRouting::IsPerspectiveProjection(orthographic)) {
			std::puts("FAIL: the orthographic terrain pass read as the player's camera");
			++g_failures;
		}
		if (!UtilityRouting::IsPerspectiveProjection(perspective)) {
			std::puts("FAIL: a perspective camera was refused");
			++g_failures;
		}

		if (!UtilityRouting::IsPerspectiveProjection(nullptr)) {
			std::puts("FAIL: a missing camera must not refuse the draw");
			++g_failures;
		}
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}

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
