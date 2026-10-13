#pragma once

// Phase 2, stage A (DESIGN.md §9): MGSV's captured meshes drawn inside Elden Ring's D3D12 frame.
//
//  - er/D3D12Hooks: Present/ResizeBuffers/ExecuteCommandLists through dummy-object vtables (system
//    DLLs only, Arxan never sees them); gives the presenting swap chain and its direct queue.
//  - er/RenderAssets: the render ring (meshes, textures) -> DEFAULT-heap resources, uploaded by the
//    Present thread's own command list.
//  - er/RenderScene: per ER frame (task Draw_Pre 144) a snapshot of the camera Elden Ring renders
//    from, the anchor (puppet feet, yaw delta) and the guest's newest DrawList.
//  - here: at Present, on Elden Ring's queue, before the original Present: back buffer PRESENT ->
//    RENDER_TARGET, our own reversed-Z depth, each DrawRecord with its own skinning matrices and the
//    anchor mapping (so the mesh lands on the debug skeleton), simple directional + ambient light
//    with the albedo/normal/SRM roles, encoded for the swap chain (sRGB, HDR10 PQ or scRGB), back to
//    PRESENT. Nothing is recorded while there is nothing to draw.
//
// Every failure turns the renderer off (logged) and leaves Elden Ring's frame untouched.

#include <string>
#include <string_view>

namespace phantom::er::Renderer
{
	bool        Install();  // DLL init, as early as possible: shader compiler + D3D12/DXGI hooks
	void        Start();    // with the other tasks: snapshot task at Draw_Pre (144)
	std::string Describe();
	// Dev console: "render" (status), "render set <key> <value>" (live [Render] tweaks), "render keys".
	std::string Command(std::string_view a_args);
}
