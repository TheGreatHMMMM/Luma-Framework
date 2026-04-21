#ifndef LUMA_GAME_CB_STRUCTS
#define LUMA_GAME_CB_STRUCTS

#ifdef __cplusplus
#include "../../../Source/Core/includes/shader_types.h"
#endif

namespace CB
{
	struct LumaGameSettings
	{
		float Dummy;
	};

	struct LumaGameData
	{
		float2 CurrJitters;
		float2 PrevJitters;
		// xy = render resolution in pixels, zw = 1/renderResolution (inverse for UV division)
		float4 RenderResolution;
		// 4 rows of (prev_VP * inv(curr_View)), composed on CPU per MV draw from the
		// engine's bound cb2 contents. Stored as 4 explicit float4 rows (rather than a
		// float4x4) to sidestep fxc row/column-major ambiguity for nested struct
		// members in cbuffers. row[i] dotted with the view-space sample produces the
		// i-th component of the previous-frame clip-space position.
		// Row 2 is unused by the MV pass (it never reads prevClip.z) and is left zero.
		float4 PrevReprojectionRow[4];
	};
}

#endif // LUMA_GAME_CB_STRUCTS
