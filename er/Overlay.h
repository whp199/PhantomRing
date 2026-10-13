#pragma once

// Draws er/Hud's model over Elden Ring's finished frame, at Present on Elden Ring's queue (after the
// renderer's own work for the frame, before the original Present): MGSV's HUD layer scaled to the back
// buffer, the fallback reticle, the corner text panels. Every element is one textured quad with
// premultiplied alpha; colours are composited as the sRGB values they are (like Elden Ring's own HUD)
// and re-encoded for an sRGB view, HDR10 PQ or scRGB back buffer.
//
// Its own command allocators, list, fence and descriptor heaps on the renderer's device; nothing is
// recorded while nothing is visible. Any D3D12 failure turns the overlay off for the session (logged).

#include "D3D12Util.h"

#include <string>

namespace phantom::er::Overlay
{
	struct Target
	{
		ID3D12Device*       device{ nullptr };
		ID3D12CommandQueue* queue{ nullptr };
		ID3D12Resource*     backBuffer{ nullptr };  // in PRESENT state
		DXGI_FORMAT         rtvFormat{ DXGI_FORMAT_UNKNOWN };
		UINT                width{ 0 }, height{ 0 };
		float               outputMode{ 0 };  // as the renderer's: 0 sRGB values in UNORM, 1 sRGB view, 2 HDR10 PQ, 3 scRGB
		float               paperWhite{ 200 };
	};

	void        OnPresent(const Target& a_target);  // Present thread, under the renderer's lock
	void        WaitIdle();                         // before a swap-chain resize (renderer's lock held)
	std::string Describe();

	// `hud shot`: the next presented frame (Elden Ring's, with everything PhantomRing drew) is copied and written as
	// PhantomRingHost.shot.bmp next to the DLL, half size. Works while Elden Ring's window is minimized or covered,
	// so a test never has to touch the player's desktop.
	void        RequestShot();
	std::string ShotStatus();
}
