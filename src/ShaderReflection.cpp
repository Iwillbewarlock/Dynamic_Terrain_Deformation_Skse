// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ShaderReflection.h"

#include <d3d11shader.h>

#include <format>

namespace Reflection
{
	namespace
	{

		uint32_t ComponentCount(uint8_t a_mask)
		{
			uint32_t count = 0;
			for (uint32_t i = 0; i < 4; ++i) {
				if (a_mask & (1u << i)) {
					++count;
				}
			}
			return count;
		}

		const char* ScalarName(uint8_t a_componentType)
		{
			switch (static_cast<D3D_REGISTER_COMPONENT_TYPE>(a_componentType)) {
			case D3D_REGISTER_COMPONENT_UINT32:
				return "uint";
			case D3D_REGISTER_COMPONENT_SINT32:
				return "int";
			case D3D_REGISTER_COMPONENT_FLOAT32:
				return "float";
			default:
				return "float";
			}
		}

		std::string MaskString(uint8_t a_mask)
		{
			std::string out;
			const char* components = "xyzw";
			for (uint32_t i = 0; i < 4; ++i) {
				if (a_mask & (1u << i)) {
					out += components[i];
				}
			}
			return out.empty() ? "-" : out;
		}
	}

	std::string SignatureElement::Semantic() const
	{
		return std::format("{}{}", semanticName, semanticIndex);
	}

	Signature ReflectOutputSignature(const void* a_bytecode, size_t a_size)
	{
		Signature signature{};

		if (!a_bytecode || a_size == 0) {
			return signature;
		}

		ID3D11ShaderReflection* reflector = nullptr;
		if (FAILED(::D3DReflect(a_bytecode, a_size, IID_ID3D11ShaderReflection,
				reinterpret_cast<void**>(&reflector))) ||
			!reflector) {
			return signature;
		}

		D3D11_SHADER_DESC desc{};
		if (FAILED(reflector->GetDesc(&desc))) {
			reflector->Release();
			return signature;
		}

		signature.elements.reserve(desc.OutputParameters);

		for (uint32_t i = 0; i < desc.OutputParameters; ++i) {
			D3D11_SIGNATURE_PARAMETER_DESC param{};
			if (FAILED(reflector->GetOutputParameterDesc(i, &param))) {
				continue;
			}

			SignatureElement element{};
			element.semanticName = param.SemanticName ? param.SemanticName : "";
			element.semanticIndex = param.SemanticIndex;
			element.registerIndex = param.Register;
			element.mask = param.Mask;
			element.componentType = static_cast<uint8_t>(param.ComponentType);

			const uint32_t components = ComponentCount(param.Mask);
			const char*    scalar = ScalarName(element.componentType);
			element.hlslType = components > 1 ? std::format("{}{}", scalar, components) : scalar;

			signature.elements.push_back(std::move(element));
		}

		reflector->Release();

		signature.valid = true;
		return signature;
	}

	uint64_t Key(const Signature& a_signature)
	{
		uint64_t key = 0xcbf29ce484222325ull;

		const auto mix = [&key](uint64_t a_value) {
			key ^= a_value;
			key *= 0x100000001b3ull;
		};

		for (const auto& e : a_signature.elements) {
			for (const char c : e.semanticName) {
				mix(static_cast<uint64_t>(static_cast<unsigned char>(c)));
			}
			mix(e.semanticIndex);
			mix(e.registerIndex);
			mix(e.mask);
			mix(e.componentType);
		}

		return key;
	}

	std::string Describe(const Signature& a_signature)
	{
		if (!a_signature.valid) {
			return "  <reflection failed>";
		}

		std::string out;
		for (const auto& e : a_signature.elements) {
			out += std::format("\n    {:<8} {:<14} reg={:<2} mask={:<4}",
				e.hlslType, e.Semantic(), e.registerIndex, MaskString(e.mask));
		}
		return out;
	}
}
