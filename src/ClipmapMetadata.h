// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include <cstdint>
#include <dxgiformat.h>

namespace Clipmap
{

	using MetadataChannel = std::uint16_t;
	inline constexpr DXGI_FORMAT kMetadataFormat = DXGI_FORMAT_R16G16_UNORM;
	inline constexpr unsigned kMetadataPixelBytes = 2 * sizeof(MetadataChannel);
}
