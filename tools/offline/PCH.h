// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace RE
{
	class UI;
	class PlayerCharacter;
	class BSShaderMaterial;
	class NiSourceTexture;
}

namespace logger
{
	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[info] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[warn] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[error] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}
}
