// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <string>
#include <vector>

namespace Reflection
{
	struct SignatureElement
	{
		std::string   semanticName;
		uint32_t      semanticIndex;
		uint32_t      registerIndex;
		uint8_t       mask;
		uint8_t       componentType;
		std::string   hlslType;

		std::string Semantic() const;
	};

	struct Signature
	{
		std::vector<SignatureElement> elements;
		bool                          valid{ false };
	};

	Signature ReflectOutputSignature(const void* a_bytecode, size_t a_size);

	std::string Describe(const Signature& a_signature);

	uint64_t Key(const Signature& a_signature);
}
