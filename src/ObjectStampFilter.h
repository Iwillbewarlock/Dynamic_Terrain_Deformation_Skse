// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once
#include <string>
#include <string_view>

namespace ObjectStampFilter
{

	inline bool IsVisualBloodProjectile(std::string_view path)
	{
		const auto slash = path.find_last_of("/\\");
		if (slash != std::string_view::npos) { path.remove_prefix(slash + 1); }
		std::string name(path);
		for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
		return name == "bloodsprayprojectileregular.nif" ||
			name == "bloodsprayprojectilesubtle.nif" ||
			name == "bloodsprayprojectilelarge.nif";
	}
}
