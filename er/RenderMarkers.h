#pragma once

// Small things PhantomRing itself draws in Elden Ring's world, in MGSV's style: the dotted throw arc and the ring
// where it lands (while Snake aims a grenade), Snake's grenades in flight and on the ground, and his placed mines
// with their blinking light, and the marks his binoculars put over Elden Ring's characters (a triangle and the distance,
// seen through walls: a slack larger than any scene). Elden Ring never sees MGSV's own objects (its grenades fly over MGSV's airport), so
// these come from the host's copies (er/Throw.h, er/Mines.h).
//
// Shapes are camera-facing quads (a dot, a lit ball) or quads lying on a surface (a ring), built on the CPU each
// frame and drawn into Elden Ring's tone-mapped scene colour before its HUD, depth-tested against its scene depth
// (the composite point of stage B, er/Renderer.cpp), or at Present over the frame when that point isn't found.
// The owners decide what is shown each frame (none in menus, cutscenes, loads, while dead).

#include "Math.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace phantom::er
{
	struct RenderSnapshot;

	struct Marker
	{
		enum class Shape : std::uint8_t
		{
			kDot,   // emissive soft disc facing the camera (arc dots, a mine's light)
			kBall,  // a small lit ball facing the camera (a grenade, a mine's body)
			kRing,  // emissive ring lying on a surface (the landing marker)
			kTriangle,  // a flat triangle pointing down, dark-edged, facing the camera (MGSV's mark over a character)
			kGlyph,     // a seven-segment digit facing the camera, dark-edged: inner = 0..9, 10 = 'm' (a marker's distance)
		};
		Shape shape{ Shape::kDot };
		Vec3  pos;                 // ER world (Havok space)
		Vec3  normal{ 0, 1, 0 };   // kRing: the surface it lies on
		float radius{ 0.05f };     // m
		float inner{ 0.0f };       // kRing: inner radius as a share of radius; kGlyph: which glyph
		float color[4]{ 1, 1, 1, 1 };  // linear RGB, alpha (straight)
		float slack{ 0.02f };          // m it may be behind Elden Ring's depth and still show (grass over a mine, bumps under a ring)
	};

	namespace RenderMarkers
	{
		// Task thread: this frame's markers of one owner (replaces what that owner gave last frame).
		enum class Owner : std::uint8_t
		{
			kThrow,
			kMines,
			kRounds,  // explosive rounds in flight (er/Rounds.h)
			kFulton,  // MGSV's Fulton balloon over an Elden Ring enemy (er/Fulton.h)
			kMarking, // binocular marks over Elden Ring characters (er/Marking.h)
			kDecoys,  // Snake's decoys (er/Decoys.h)
			kCount,
		};
		void Set(Owner a_owner, std::vector<Marker> a_markers);
		void Clear();
		// RenderScene::Capture: everything set for this frame.
		std::vector<Marker> Collect();

		// Render thread: one vertex per quad corner (6 per marker), in ER world.
		struct Vertex
		{
			float pos[3];
			float corner[2];   // -1..1 across the quad
			float color[4];    // linear, straight alpha
			float shape[4];    // x shape, y ring inner share, z view z of the centre, w depth slack (m)
		};
		static_assert(sizeof(Vertex) == 13 * 4);
		// Quads for a_markers seen from a camera (a_right/a_up its axes): dots and balls face it, rings lie on their
		// surface, nudged a little toward the camera so the ground's own depth doesn't eat them.
		std::vector<Vertex> Build(std::span<const Marker> a_markers, const Vec3& a_camPos, const Vec3& a_right, const Vec3& a_up);

		std::string Describe();
	}
}
