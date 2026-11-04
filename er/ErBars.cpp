#include "ErBars.h"

#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Log.h"
#include "Mem.h"
#include "Puppet.h"
#include "Singletons.h"

#include <format>

#include <windows.h>

namespace phantom::er::ErBars
{
	namespace
	{
		constexpr std::uint32_t kHudState = 0x78;  // CSFeManHudState: 0 HideAll (HP/FP/stamina), 1 ShowAll, 2 popup menu, 3 Default
		constexpr std::uint8_t  kHideAll = 0;
		constexpr std::uint8_t  kDefault = 3;

		std::string   g_override;  // console: "" = ini
		bool          g_hidAll{ false };   // we set hud_state 0 (put 3 back when Snake stops)
		std::uint64_t g_frames{ 0 }, g_skipped{ 0 };
		bool          g_warned{ false };
		std::string   g_state{ "not used yet" };
		std::uint8_t* g_feMan{ nullptr };
		// (0.7.0 integration) hud_state 0 also hides Elden Ring's in-game HUD as CSMenuMan UI state 7 reads it, the signal the
		// interactions (er/Interaction.h) and the "Auto" HUD detector follow: live, every takeover counted as "the HUD hid with
		// nothing on screen", the press-less screens went off for the session, and a rest at a grace ended after its 3.5 s
		// minimum with the grace menu still up (Snake had the pad: B reloaded). So: HidesHud() tells er/Host.cpp to ignore UI
		// state 7 while (and kStateLagMs after) we hide, and after one of Elden Ring's screens the bars come back only kRehideMs
		// later, so the screen that follows one (Melina's question after her talk, the grace's next screen) still shows its hide.
		constexpr std::uint64_t kRehideMs = 1500;
		constexpr std::uint64_t kStateLagMs = 300;
		std::uint64_t g_notSnakeAt{ 0 };  // the last frame Snake's screen wasn't up
		std::uint64_t g_putBackAt{ 0 };   // when we last put hud_state back to 3

		std::string_view Mode()
		{
			const std::string_view m = g_override.empty() ? std::string_view(Cfg().overlay.erBars) : std::string_view(g_override);
			if (m == "auto") {
				return "none";
			}
			return m;
		}

		std::uint8_t* FeMan()
		{
			auto* slot = SingletonSlot("CSFeMan");
			auto* fe = slot ? SafePtr(slot) : nullptr;
			if (fe != g_feMan) {
				g_feMan = fe;
				g_hidAll = false;
				if (fe && game::RttiName(fe) != "CS::CSFeManImp") {
					if (!g_warned) {
						g_warned = true;
						Log::Warn("Elden Ring's bars: CSFeMan at {} is a {} (not CS::CSFeManImp) -- its bars are left alone", static_cast<void*>(fe), game::RttiName(fe));
					}
					g_feMan = nullptr;
				}
			}
			return g_feMan;
		}

		void PutBack(std::uint8_t* a_fe)
		{
			if (g_hidAll && a_fe && SafeRead<std::uint8_t>(a_fe + kHudState).value_or(0xFF) == kHideAll) {
				SafeWrite<std::uint8_t>(a_fe + kHudState, kDefault);
				g_putBackAt = ::GetTickCount64();
			}
			g_hidAll = false;
		}
	}

	void Apply(const Frame& a_f)
	{
		auto*      fe = FeMan();
		const auto mode = Mode();
		// Snake's screen only: not in Elden Ring's menus, a load, a cutscene, a death, Tarnished mode. Snake on a ladder (Elden Ring
		// climbs, the puppet held: Puppet::ClimbVisual) is still his screen, as for MGSV's HUD layer (er/Hud.cpp; review 0.7.0: the
		// Tarnished's bars showed next to MGSV's HUD for every climb).
		const bool snake = (Puppet::Active() || Puppet::ClimbVisual()) && a_f.inGame && !a_f.menuOpen && !a_f.loading && !a_f.cutscene &&
		                   !a_f.dead && !a_f.dying && !Puppet::Dying();
		if (!fe) {
			g_state = "no CSFeMan";
			return;
		}
		const auto now = ::GetTickCount64();
		if (!snake) {
			g_notSnakeAt = now;
		}
		if (!snake || mode == "all") {
			PutBack(fe);
			g_state = snake ? "all three bars (as Elden Ring has them)" : "Elden Ring's (Snake isn't playing)";
			return;
		}
		if (g_notSnakeAt && now - g_notSnakeAt < kRehideMs && !g_hidAll) {
			g_state = "Elden Ring's for a moment after one of its screens";
			return;
		}
		if (mode == "none") {
			const auto hud = SafeRead<std::uint8_t>(fe + kHudState).value_or(0xFF);
			if (hud == kDefault) {
				SafeWrite<std::uint8_t>(fe + kHudState, kHideAll);
				g_hidAll = true;
			}
			++g_frames;
			g_state = std::format("HP, FP and stamina hidden (hud_state {} -> 0)", hud);
			return;
		}
		PutBack(fe);
		g_state = "all three bars (unknown mode)";
	}

	bool HidesHud()
	{
		const auto now = ::GetTickCount64();
		return (g_hidAll && g_feMan && SafeRead<std::uint8_t>(g_feMan + kHudState).value_or(0xFF) == kHideAll) ||
		       (g_putBackAt && now - g_putBackAt < kStateLagMs);
	}

	std::string Describe()
	{
		return std::format("Elden Ring's bars: [Overlay] er_bars = {} -> {} | {} | {} frames written, {} skipped", g_override.empty() ? Cfg().overlay.erBars : g_override + " (console)",
			Mode(), g_state, g_frames, g_skipped);
	}

	std::string SetMode(std::string_view a_mode)
	{
		if (a_mode == "status") {
			return Describe();
		}
		if (a_mode != "none" && a_mode != "all" && a_mode != "auto" && a_mode != "ini") {
			return "hud bars none|all|auto|ini";
		}
		g_override = a_mode == "ini" ? "" : std::string(a_mode);
		return Describe();
	}
}
