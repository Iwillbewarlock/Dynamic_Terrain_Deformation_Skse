// SPDX-License-Identifier: LicenseRef-NMN-Proprietary
// Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved.

#pragma once

namespace StampShapes
{

	bool Initialize();

	void Release();

	void Retry();

	bool Ready();

	ID3D11ShaderResourceView* View();

	void Analyse();

	void RequestAnalysis();
}
