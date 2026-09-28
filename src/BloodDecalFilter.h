// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once
#include <string_view>

namespace BloodDecalFilter
{

	inline bool Matches(std::string_view path, std::string_view prefixes)
	{
		const auto slash = path.find_last_of("/\\");
		if (slash != std::string_view::npos) { path.remove_prefix(slash + 1); }
		const auto equalFolded = [](std::string_view a, std::string_view b) {
			if (a.size() != b.size()) { return false; }
			for (size_t i = 0; i < a.size(); ++i) {
				const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
				if (lower(a[i]) != lower(b[i])) { return false; }
			}
			return true;
		};
		if (path.size() < 4 || !equalFolded(path.substr(path.size() - 4), ".dds")) { return false; }
		while (!prefixes.empty()) {
			const auto end = prefixes.find(',');
			auto token = prefixes.substr(0, end);
			while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) { token.remove_prefix(1); }
			while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) { token.remove_suffix(1); }
			if (!token.empty() && path.size() >= token.size() &&
				equalFolded(path.substr(0, token.size()), token)) { return true; }
			if (end == std::string_view::npos) { break; }
			prefixes.remove_prefix(end + 1);
		}
		return false;
	}
}
