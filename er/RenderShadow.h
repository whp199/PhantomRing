#pragma once

// Stage C shadows: Snake (and his buddy) drawn depth-only into Elden Ring's shadow atlas -- each cascade
// viewport with that cascade's own light view (er/RenderBind finds it, er/RenderView copies it) and the
// depth function and bias of Elden Ring's own shadow casters (er/RenderPso) -- after Elden Ring rendered
// the cascades and before its shadow-mask pass reads them, so Snake casts the sun's shadow on its world
// (and on himself) exactly like the Tarnished.

#include "RenderBind.h"
#include "RenderShared.h"

#include <string>
#include <vector>

namespace phantom::er::RenderShadow
{
	bool Init(ID3D12Device* a_device, ID3D12RootSignature* a_root);
	// a_views[i]'s constants are in RenderView slot a_firstSlot + i. Our heap, root signature and frame
	// constants (FrameCB.misc.w = 1) are bound by the caller. Returns the draws recorded.
	std::uint32_t Record(render::Recording& a_rec, const RenderSnapshot& a_s, const std::vector<RenderBind::View>& a_views, UINT a_firstSlot);
	void          Reset();
	std::string   Describe();

	// Elden Ring's sun-cascade atlas (VERIFIED live, 4K: 4096x2048 with views at (0,0) 2048^2, (2048,0) 1024^2,
	// (3072,0) 1024^2): twice as wide as high, one square view of its full height at the origin. Its cascades are
	// rendered every frame. The other R16 atlases (local lights, environment captures: 1-3 views a frame) may be
	// rendered once and reused, and would keep Snake's silhouette from that frame: Snake is drawn only into
	// the cascade atlas, or an atlas Elden Ring cleared in full this frame.
	bool SunCascadeAtlas(UINT a_width, UINT a_height, const D3D12_VIEWPORT* a_views, std::size_t a_count);
}
