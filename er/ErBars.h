#pragma once

// Elden Ring's own HP / FP / stamina bars while Snake plays (roadmap 2.13; [Overlay] er_bars; 0.7.0).
//
// Snake's life, ammo and items are MGSV's HUD layer; Elden Ring's FP and stamina bars belonged to the hidden Tarnished and meant
// nothing while Snake played (its HP bar mirrors Snake's life, [Puppet] mirror_life, but MGSV's HUD shows his life too).
//  - none (the default, auto): all three go, through CSFeMan's own HUD switch (+0x78 hud_state, fromsoftware-rs CSFeManHudState:
//    0 = HP, FP and stamina hidden, 1 shown, 2 a popup menu's, 3 the default) -- set only while it is the default (3), so Elden
//    Ring's menus (1, 2) keep theirs, and put back when Snake stops (Tarnished mode, a menu, a load, a death);
//  - all: as Elden Ring has them (before 0.7.0).
// FP and stamina alone could not be hidden (live 2026-11-03, the HUD forced On): CSFeMan's front-end values (FrontEndViewValues
// fp / fp max / stamina / stamina max, +0x98..+0xB8) and its Auto HUD's per-element "shown" bytes (+0x4EB8: 1 HP, 2 FP, 3 stamina)
// written 0 every frame, after CSMenuMan's update (task groups 126 and 107), left both bars drawn full: Elden Ring's bars take
// their values elsewhere. CSFeMan is found by its singleton name and checked by RTTI (CS::CSFeManImp); anything else leaves the
// bars alone (logged once).

#include <string>

namespace phantom::er
{
	struct Frame;

	namespace ErBars
	{
		void        Apply(const Frame& a_frame);  // the camera task (group 107)
		// Our hud_state 0 is what hides Elden Ring's HUD now (or did a moment ago): CSMenuMan UI state 7 then says nothing about
		// its screens (er/Host.cpp HudHidden ignores it).
		bool        HidesHud();
		std::string Describe();
		// console `hud bars none|all|auto|ini`
		std::string SetMode(std::string_view a_mode);
	}
}
