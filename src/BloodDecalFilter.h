// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace BloodDecalFilter
{

	using Prefixes = std::vector<std::string>;

	inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

	// Splits, trims and lower-cases the list once, so that matching a path allocates nothing.
	inline Prefixes Parse(std::string_view prefixes)
	{
		Prefixes parsed;
		while (!prefixes.empty()) {
			const auto end = prefixes.find(',');
			auto token = prefixes.substr(0, end);
			while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) { token.remove_prefix(1); }
			while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) { token.remove_suffix(1); }
			std::string prefix(token);
			for (auto& c : prefix) { c = Lower(c); }
			if (!prefix.empty()) { parsed.push_back(std::move(prefix)); }
			if (end == std::string_view::npos) { break; }
			prefixes.remove_prefix(end + 1);
		}
		return parsed;
	}

	inline bool Matches(std::string_view path, const Prefixes& prefixes)
	{
		const auto slash = path.find_last_of("/\\");
		if (slash != std::string_view::npos) { path.remove_prefix(slash + 1); }
		constexpr std::string_view extension = ".dds";
		if (path.size() < extension.size()) { return false; }
		for (size_t i = 0; i < extension.size(); ++i) {
			if (Lower(path[path.size() - extension.size() + i]) != extension[i]) { return false; }
		}
		for (const auto& prefix : prefixes) {
			if (prefix.size() > path.size()) { continue; }
			size_t i = 0;
			while (i < prefix.size() && Lower(path[i]) == prefix[i]) { ++i; }
			if (i == prefix.size()) { return true; }
		}
		return false;
	}

	inline bool Matches(std::string_view path, std::string_view prefixes)
	{
		return Matches(path, Parse(prefixes));
	}
}
