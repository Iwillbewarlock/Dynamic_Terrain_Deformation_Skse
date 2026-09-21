// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once
#include <string>
#include <string_view>

namespace BloodDecalFilter
{

	inline bool Matches(std::string_view path, std::string_view prefixes)
	{
		const auto slash = path.find_last_of("/\\");
		if (slash != std::string_view::npos) { path.remove_prefix(slash + 1); }
		std::string name(path);
		for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
		if (!name.ends_with(".dds")) { return false; }
		while (!prefixes.empty()) {
			const auto end = prefixes.find(',');
			auto token = prefixes.substr(0, end);
			while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) { token.remove_prefix(1); }
			while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) { token.remove_suffix(1); }
			std::string prefix(token);
			for (auto& c : prefix) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
			if (!prefix.empty() && name.starts_with(prefix)) { return true; }
			if (end == std::string_view::npos) { break; }
			prefixes.remove_prefix(end + 1);
		}
		return false;
	}
}
