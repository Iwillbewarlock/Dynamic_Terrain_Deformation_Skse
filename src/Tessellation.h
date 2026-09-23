// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "ShaderReflection.h"

namespace Tessellation
{

	inline constexpr uint32_t kMaterialSlot = 11;

	inline constexpr uint32_t kActorPaintSlot = 10;

	enum class Mode
	{
		kLandscape,
		kActorPaint,

		kStaticProbe,

		kMeshRaise,

		kBloodDecal,
	};

	bool WantsDisplacement(Mode a_mode);
	bool WantsSubdivision(Mode a_mode);

	struct DrawMaterial
	{

		int revealLayer{ -1 };

		float meshTopZ{ 0.0f };
		float meshBand{ 0.0f };

		int snowLayer{ -1 };

		float strengthScale{ 1.0f };

		float coatColour[3]{ 0.0f, 0.0f, 0.0f };
		float coatAmount{ 0.0f };

		float coatCling{ 0.0f };

		float coatGain{ 1.0f };

		float coatFeetZ{ 0.0f };
		float coatHeight{ 0.0f };
	};

	bool BeginDraw(uint64_t a_vertexDesc, const Reflection::Signature& a_signature,
		Mode a_mode, const DrawMaterial& a_material);

	void EndDraw();

	bool Active();

	void VertexShaderBound(ID3D11DeviceContext*, ID3D11VertexShader*);
	ID3D11RasterizerState* RasterizerFor(ID3D11DeviceContext*, ID3D11RasterizerState*);

	void Reset();
	void PrepareFrame();

	// Per-frame value for the screen-space cap (ClipmapWindow.Screen.x): internal viewport
	// height / 2 / TessellationScreenPixels, or 0 while the cap is off or unmeasured.
	float ScreenScale();
}
