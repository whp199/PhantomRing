#include "Host.h"

#include "Actors.h"
#include "Addresses.h"
#include "Buddy.h"
#include "BuddyOrders.h"
#include "Calibration.h"
#include "CameraDriver.h"
#include "Chord.h"
#include "CombatLogic.h"
#include "Config.h"
#include "Console.h"
#include "DebugDraw.h"
#include "Events.h"
#include "Fulton.h"
#include "GeomProbe.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Interaction.h"
#include "Kbm.h"
#include "KeyGuide.h"
#include "Ladder.h"
#include "Link.h"
#include "Log.h"
#include "Mem.h"
#include "MenuWatch.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Progression.h"
#include "Puppet.h"
#include "QuickItem.h"
#include "Renderer.h"
#include "Revival.h"
#include "Tasks.h"
#include "WorldShift.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <windows.h>

namespace phantom::er::Host
{
	namespace
	{
		Frame         g_frame;
		bool          g_everInGame{ false };
		std::uint8_t* g_seenPlayer{ nullptr };
		std::uint64_t g_seenSince{ 0 };
		bool          g_entryUnreliable{ false };
		Interaction   g_interaction;
		std::uint32_t g_promptPresses{ 0 };
		std::int32_t  g_interactionPrompt{ -1 };
		std::uint64_t g_releaseUntil{ 0 };  // the puppet stepped aside for an ER-driven walk (the mist) until then
		bool          g_hudSeen{ false };  // the HUD state was visible since the last load (else it is off in ER's settings)
		bool          g_hudWasHidden{ false };
		// The HUD's last hide came with one of Elden Ring's screens (a prompt just before it: a dialog, a pickup; a rest; a menu;
		// a cutscene). Tarnished mode keeps the chord from giving Snake back only under such screens (live pass B 2026-10-15: in
		// Tarnished mode the HUD state hid with nothing on screen -- after a map press Elden Ring refused, an enemy near -- and
		// View + Start then only woke it: Snake came back on the second try. Its "Auto" HUD fading doesn't touch this state).
		bool          g_hudHideScreen{ false };
		constexpr std::uint64_t kScreenPromptMs = 2000;
		std::uint64_t g_promptSeenAt{ 0 };  // an interaction prompt was on screen then
		std::int32_t  g_promptSeen{ -1 };   // ... this one
		constexpr std::uint64_t kDialogPromptMs = 1000;
		std::uint64_t g_interactionEndedAt{ 0 };  // the last interaction ended then (its HUD came back)
		constexpr std::uint64_t kChainMs = 1000;
		bool          g_hudUpAtPress{ true };
		bool          g_erMenuOpen{ false };  // an Elden Ring menu of its own (not the interaction folded into menuOpen)
		// Sitting at a grace: from a rest until the player is kGraceRadius away from where he rested (or a load, a death).
		// Every screen of the grace hides Elden Ring's HUD, but not always within kChainMs of the last: live, "Pass time"
		// showed the HUD 4.7 s through its fade, then the grace menu again with Snake holding the pad (B reloaded his rifle;
		// nothing closed the menu). While he sits there, Elden Ring's HUD hiding is always the grace's next screen.
		bool          g_atGrace{ false };
		bool          g_mountUnknownLogged{ false };
		Vec3          g_gracePos{};
		constexpr float kGraceRadius = 2.0f;
		// Press-less starts (dialog, chained, graceScreen) and Elden Ring's "Auto" HUD (er/Interaction.h AutoHud).
		AutoHud       g_autoHud;
		bool          g_presslessStart{ false };  // the running interaction began with no press
		Vec3          g_presslessFrom{};          // where the Tarnished stood then: a screen keeps him there

		// Tarnished mode (roadmap 1.1): View + Start held together (er/Chord.h) hands the controller to the Tarnished --
		// flasks, spirit ashes, Torrent, jumps, ladders -- through the puppet's own stop/start (Puppet::Hold::kTarnished), and
		// back: Snake starts again where the Tarnished stands (the puppet's start anchors him there; D-Walker is
		// anchor-relative, so it comes along).
		bool          g_tarnished{ false };
		std::uint32_t g_chordsSeen{ 0 };
		float         g_tarnishedHpFrac{ 1 };   // the HP bar (Snake's life) when the Tarnished took over
		std::uint32_t g_tarnishedAttach{ 0 };   // the guest attach the last kHEvTarnished went to
		std::uint64_t g_tarnishedToggles{ 0 };
		// A rest at a grace (kHEvRested 0): Elden Ring's HUD hiding within kRestWindowMs of its "Rest at site of grace" prompt
		// ([Input] rest_prompts) -- the sit-down hides it ~2.5 s after the press (VERIFIED live: 6101, then the grace menu).
		constexpr std::uint64_t kRestWindowMs = 6000;
		std::uint64_t g_restPromptAt{ 0 };
		bool          g_restHudHidden{ false };
		bool          g_restPromptWasUp{ false };
		bool          g_restButtonDown{ false };
		bool          g_restPress{ false };      // this frame: Snake's prompt press went to Elden Ring on a rest prompt
		std::uint64_t g_restQuietUntil{ 0 };     // a rest was counted: the HUD hiding after it is the same rest
		std::uint64_t g_rests{ 0 };
		// Ladders (er/Ladder.h, roadmap 1.7): an automatic Tarnished mode while Elden Ring climbs one with him.
		LadderHandOver g_ladder;
		bool           g_ladderPress{ false };     // this frame: Snake's prompt press on a ladder prompt went to Elden Ring
		std::int32_t   g_ladderPrompt{ -1 };
		float          g_ladderHpFrac{ 1 };        // the HP bar when the Tarnished took the ladder
		bool           g_ladderUnknownLogged{ false };
		int            g_ladderState{ -1 };        // Elden Ring's ladder state last frame (logged on every change)
		std::uint64_t  g_ladderClimbs{ 0 };
		std::uint64_t  g_ladderNoClimbs{ 0 };
		std::uint64_t  g_ladderSince{ 0 };        // when the hand-over started
		bool           g_ladderUsable{ false };    // the ladder state reads and ladder_prompts isn't "none": the hand-over is on
		Vec3           g_ladderFrom{};
		// Snake climbs it himself ([Ladder] snake, protocol v15): kHEvClimb out, MGSV's kGuestClimbing back, Snake drawn on it.
		ClimbPace      g_climbPace;
		bool           g_climbVisual{ false };     // this frame: Snake is drawn on the ladder, the Tarnished hidden
		bool           g_guestClimbSeen{ false };  // kGuestClimbing since this climb's kHEvClimb a = 1
		bool           g_guestClimbWasOff{ false };  // ... and it was down at some point since the hand-over started (the last climb's
		                                             // flag, still up while MGSV takes Snake home, is not this climb's)
		std::uint64_t  g_climbSentAt{ 0 };         // when this climb's first kHEvClimb went out
		bool           g_climbEndedEarly{ false };  // kHEvClimb a = 0 went out at Elden Ring's EndTop / EndBottom -> none (the settle runs)
		bool           g_stickLatchedAtEnd{ false };  // the left stick's latch was taken at EndTop / EndBottom (not again at the end)
		std::uint64_t  g_climbEndedEarlyMs{ 0 };
		bool           g_guestClimbLate{ false };  // ...and kGuestClimbing never came within ladder_guest_wait_ms (logged once a climb)
		std::uint64_t  g_guestClimbs{ 0 };         // climbs MGSV played (kGuestClimbing seen)
		std::uint64_t  g_snakeClimbs{ 0 };         // climbs Snake was drawn on
		// Snake's drawn place eases from where the puppet had him to the Tarnished's on the ladder (a "Descend" moves the Tarnished
		// up to ~1 m first, AlignForDescent; Elden Ring's own start turns him round).
		constexpr float kClimbBlendS = 0.3f;
		Anchor         g_climbFrom{};
		std::uint64_t  g_climbFromMs{ 0 };

		bool IsLadderPrompt(std::int32_t a_id)
		{
			const auto& l = Cfg().ladderPrompts;
			return a_id >= 0 && std::ranges::find(l, a_id) != l.end();
		}

		// A ladder prompt the hand-over takes care of (with the ladder module unverified its press stays a plain interaction).
		bool HandOverLadder(std::int32_t a_id) { return g_ladderUsable && IsLadderPrompt(a_id); }

		bool IsRestPrompt(std::int32_t a_id)
		{
			const auto& rest = Cfg().restPrompts;
			return a_id >= 0 && std::ranges::find(rest, a_id) != rest.end();
		}

		void PushEvent(proto::HostEventType a_type, std::uint32_t a_a, std::uint32_t a_b, std::uint64_t a_frame, float a_v0 = 0.0f)
		{
			proto::HostEvent e{};
			e.type = a_type;
			e.a = a_a;
			e.b = a_b;
			e.frame = static_cast<std::uint32_t>(a_frame);
			e.v0[0] = a_v0;
			if (!Link::Get().PushHostEvent(e)) {
				Log::Debug("host event {} dropped (guest ring full or detached)", static_cast<std::uint32_t>(a_type));
			}
		}

		float HpFrac(const Frame& a_f)
		{
			if (!a_f.player) {
				return 1.0f;
			}
			return static_cast<float>(game::Hp(a_f.player)) / static_cast<float>(std::max(1, game::MaxHp(a_f.player)));
		}

		// What the Tarnished went through while he had the controller (Tarnished mode, a ladder): the HP bar's net change since
		// a_fromFrac, as the same share of Snake's life (the bar is Snake's life again from the next frame on).
		std::string CarryHp(const Frame& a_f, float a_fromFrac)
		{
			if (!a_f.player || !a_f.guestReady || a_f.guest.lifeMax == 0 || !Cfg().mirrorLife) {
				return "nothing to carry over";
			}
			const auto&        c = Cfg();
			const std::int32_t maxHp = std::max(1, game::MaxHp(a_f.player));
			const auto         diff = static_cast<std::int32_t>(std::lround((HpFrac(a_f) - a_fromFrac) * static_cast<float>(maxHp)));
			if (diff > 0 && c.forwardHeals) {
				double     carry = 0;
				const auto amount = combat::HealForGuest(diff, maxHp, a_f.guest.lifeMax, c.guestLifeScale, carry);
				PushEvent(proto::kHEvHeal, amount, static_cast<std::uint32_t>(diff), a_f.index);
				return std::format("healed {} HP as the Tarnished -> Snake +{} life (kHEvHeal {})", diff, static_cast<int>(amount * c.guestLifeScale), amount);
			}
			if (diff < 0) {
				const auto amount = combat::HurtForGuest(-diff, maxHp, a_f.guest.lifeMax, c.guestLifeScale, c.hurtShare, c.hurtMult);
				PushEvent(proto::kHEvHurt, amount, proto::kHurtOther, a_f.index);
				return std::format("lost {} HP as the Tarnished -> Snake -{} life (kHEvHurt {})", -diff, static_cast<int>(amount * c.guestLifeScale), amount);
			}
			return "nothing to carry over";
		}

		void SetTarnished(const Frame& a_f, bool a_on, std::string_view a_why)
		{
			if (a_on == g_tarnished) {
				return;
			}
			g_tarnished = a_on;
			++g_tarnishedToggles;
			const float frac = HpFrac(a_f);
			if (a_on) {
				// On a ladder the Tarnished already had the controller: what happened since he took the ladder counts from there.
				g_tarnishedHpFrac = g_ladder.Holding() ? g_ladderHpFrac : frac;
				Puppet::SetHold(Puppet::Hold::kTarnished, true);
				PushEvent(proto::kHEvTarnished, 1, 0, a_f.index);
				g_tarnishedAttach = Link::Get().AttachCount();
				Hud::Notify("Tarnished mode -- View+Start to return to Snake", Hud::Tone::kGood);
				Log::Info("Tarnished mode ON ({}): the Tarnished has the controller, Snake parks (kHEvTarnished 1); HP bar {:.0f}% (toggle #{})", a_why,
					frac * 100.0f, g_tarnishedToggles);
				return;
			}
			const std::string carried = CarryHp(a_f, g_tarnishedHpFrac);
			Puppet::SetHold(Puppet::Hold::kTarnished, false);
			PushEvent(proto::kHEvTarnished, 0, 0, a_f.index);
			Log::Info("Tarnished mode OFF ({}): Snake takes over where the Tarnished stands (kHEvTarnished 0); HP bar {:.0f}% -> {:.0f}%: {} (toggle #{})", a_why,
				g_tarnishedHpFrac * 100.0f, frac * 100.0f, carried, g_tarnishedToggles);
		}

		// FrameBegin, before the puppet decides: the chord toggles Tarnished mode; a new guest hears that it is on.
		void UpdateTarnished(const Frame& a_f)
		{
			if (const auto chords = InputBridge::Chords(); chords != g_chordsSeen) {
				g_chordsSeen = chords;
				// Snake takes over on foot: on Torrent or a ladder the puppet would start under a rider or a climber (Elden
				// Ring's own state for the Tarnished, never Snake's).
				const auto on = g_tarnished ? game::PlayerMounted(a_f.player) : game::Mounted::kNone;
				if (ChordRefusedWhileDying(Puppet::Dying())) {
					// er/Revival.h: on in a death's first frames it stopped the puppet with the Tarnished on screen; off, Snake would
					// take over a dead Tarnished. After the respawn it works again.
					Log::Info("input: View+Start during a death -- ignored (Tarnished mode {}; the chord works again after the respawn)",
						g_tarnished ? "stays on" : "stays off");
				} else if (on == game::Mounted::kTorrent || on == game::Mounted::kLadder) {
					const char* what = on == game::Mounted::kTorrent ? "Torrent" : "the ladder";
					Hud::Notify(std::format("Tarnished mode: get off {} first, then View+Start", what), Hud::Tone::kWarn);
					Log::Info("input: View+Start in Tarnished mode while the Tarnished is on {} -- Snake waits until he is on foot", what);
				} else if (g_tarnished) {
					if (on == game::Mounted::kUnknown && !g_mountUnknownLogged) {
						g_mountUnknownLogged = true;
						Log::Info("Tarnished mode: can't tell whether the Tarnished rides or climbs (ride/ladder modules not as verified) -- "
							"Snake takes over anyway");
					}
					SetTarnished(a_f, false, "View+Start");
				} else if (Puppet::Active()) {
					SetTarnished(a_f, true, "View+Start");
				} else if (g_ladder.Holding()) {
					// On a ladder the Tarnished already has the controller: the chord keeps him after it (Snake stays parked).
					SetTarnished(a_f, true, "View+Start on a ladder");
				} else {
					Log::Info("input: View+Start with Snake not playing -- ignored");
				}
			}
			if (g_tarnished && a_f.guestAlive && Link::Get().AttachCount() != g_tarnishedAttach) {
				g_tarnishedAttach = Link::Get().AttachCount();
				PushEvent(proto::kHEvTarnished, 1, 0, a_f.index);  // a guest that (re)attached meanwhile parks too
				Log::Info("Tarnished mode: MGSV attached again -> kHEvTarnished 1");
			}
		}

		// kHEvRested 0: a rest at a grace (the respawn's kHEvRested 1 is er/Puppet.cpp's). "Rest at site of grace" rests on the
		// press (Elden Ring sits down, heals and refills the flasks; live, Melina came and talked instead of the grace menu, and the
		// HUD never hid): Snake's prompt press, or the Tarnished's own (Tarnished mode: the bridge still sees the real pad).
		// A rest Elden Ring started otherwise (its keyboard) still hides its HUD a moment after the prompt was up.
		void UpdateRest(const Frame& a_f)
		{
			const auto now = ::GetTickCount64();
			const bool up = IsRestPrompt(a_f.prompt);
			if (up) {
				g_restPromptAt = now;
			}
			const bool down = (InputBridge::LastButtons() & Cfg().promptButton) != 0;
			const bool ownPress = down && !g_restButtonDown && !Puppet::Active() && (up || g_restPromptWasUp) && a_f.inGame && !a_f.menuOpen;
			g_restButtonDown = down;
			g_restPromptWasUp = up;
			// Not when one of Elden Ring's menus hid the HUD (its map or menu opened next to a grace: live, the map hides it too).
			const bool hidden = a_f.hudHidden && a_f.inGame && !a_f.cutscene && !a_f.dead && !a_f.dying;
			const bool viaHud = hidden && !g_restHudHidden && !g_erMenuOpen && g_restPromptAt && now - g_restPromptAt < kRestWindowMs && now >= g_restQuietUntil;
			g_restHudHidden = hidden;
			if (!(g_restPress || ownPress || viaHud)) {
				return;
			}
			++g_rests;
			PushEvent(proto::kHEvRested, 0, 0, a_f.index);
			Fulton::OnEnemiesBack("a rest at a grace");
			Log::Info("ER: rested at a grace ({}; Elden Ring refills the flasks) -> kHEvRested 0: Snake's ammo refilled (rest #{})",
				g_restPress ? "Snake's Y on its rest prompt" :
				ownPress    ? "the Tarnished's Y on its rest prompt" :
							  std::format("its HUD hid {:.1f} s after the rest prompt", static_cast<double>(now - g_restPromptAt) / 1000.0),
				g_rests);
			g_restPromptAt = 0;
			g_restQuietUntil = now + kRestWindowMs;
			if (a_f.player) {
				g_atGrace = true;
				g_gracePos = game::Position(a_f.player);
			}
		}

		// Ladders (er/Ladder.h): Snake's press on a ladder prompt, or a ladder state while he plays, hands the Tarnished to Elden Ring
		// for the climb (Puppet::Hold::kLadder), until he is off it; then Snake takes over where he got off. What the Tarnished
		// went through meanwhile (an arrow, a fall off it) is carried over to Snake as in Tarnished mode.
		// kHEvClimb (protocol v15): a = 1 the climb started (again: it turned or changed pace), 0 it ended; b = 0 up / 1 down;
		// v0[0] = its vertical speed (m/s).
		void SendClimb(const Frame& a_f, const ClimbPace::Out& a_out, int a_state)
		{
			if (!a_out.send) {
				return;
			}
			PushEvent(proto::kHEvClimb, a_out.on ? 1u : 0u, a_out.down ? 1u : 0u, a_f.index, a_out.speed);
			const bool first = a_out.on && !g_climbSentAt;
			if (first) {
				g_climbSentAt = ::GetTickCount64();
				Hud::HideLayerFor(Cfg().overlay.climbHideMs);  // MGSV's last prompt as it stages Snake at its ladder: not over Elden Ring's frame
			}
			if (a_out.on) {
				Log::Info("ladder: kHEvClimb a=1 b={} ({} at {}; ER ladder state {} {}){}", a_out.down ? 1 : 0,
					a_out.motion > 0 ? "up" : a_out.motion < 0 ? "down" : "still on it",
					a_out.speed >= 0.0f ? std::format("{:.2f} m/s", a_out.speed) : std::string("MGSV's own pace"), a_state, LadderStateName(a_state),
					first ? " -- MGSV plays Snake's climb" : "");
			} else {
				Log::Info("ladder: kHEvClimb a=0 (the climb is over; {} events)", g_climbPace.Sent());
			}
		}

		// Snake on the ladder ([Ladder] snake): drawn where the Tarnished is (his feet, facing the ladder as he does), the
		// Tarnished hidden -- while the hand-over holds, Snake doesn't play Tarnished mode and MGSV is there to draw him.
		bool WantClimbVisual(const Frame& a_f)
		{
			const auto& mode = Cfg().ladderSnake;
			if (mode == "off" || !g_ladder.Holding() || g_tarnished || !a_f.guestReady || !a_f.player || !a_f.inGame) {
				return false;
			}
			if (mode == "auto" && !g_guestClimbSeen && g_climbSentAt && ::GetTickCount64() - g_climbSentAt > static_cast<std::uint64_t>(std::max(0, Cfg().ladderGuestWaitMs))) {
				return false;  // MGSV doesn't play the climb: the Tarnished climbs as before
			}
			return true;
		}

		// After Elden Ring's physics (it moved the Tarnished up the rungs): where Snake is drawn on the ladder.
		void UpdateClimbAnchor(const Frame& a_f)
		{
			if (!g_climbVisual || !a_f.player || Puppet::Active()) {
				return;
			}
			Anchor a;
			a.axes.mirrorX = Cfg().mirrorX;
			a.axes.mgsvYawSign = Cfg().mgsvYawSign;
			a.axes.erForward = Calibration::ErForward();
			a.axes.mgsvCamForward = Cfg().mgsvCamForward;
			const Quat q = game::Orientation(a_f.player);
			a.delta = ChooseDelta(q, a_f.guest.rootYaw, a.axes);  // Snake's MGSV facing turned onto the Tarnished's (the ladder)
			const Vec3 fwd = Normalize(Vec3{ ErForward(q, a.axes).x, 0.0f, ErForward(q, a.axes).z });
			const Vec3 right{ fwd.z, 0.0f, -fwd.x };
			const auto& off = Cfg().ladderOffset;
			a.pe = game::Position(a_f.player) + fwd * off[0] + Vec3{ 0.0f, off[1], 0.0f } + right * off[2];
			a.pm = Vec3{ a_f.guest.rootPos[0], a_f.guest.rootPos[1] - Cfg().mgsvRootOffsetY, a_f.guest.rootPos[2] };
			if (g_climbFromMs) {
				const float t = static_cast<float>(::GetTickCount64() - g_climbFromMs) / (1000.0f * kClimbBlendS);
				if (t < 1.0f && Finite(g_climbFrom.pe) && Length(a.pe - g_climbFrom.pe) < 3.0f) {
					const float k = t <= 0.0f ? 0.0f : t * t * (3.0f - 2.0f * t);
					a.pe = g_climbFrom.pe + (a.pe - g_climbFrom.pe) * k;
					a.delta = WrapPi(g_climbFrom.delta + WrapPi(a.delta - g_climbFrom.delta) * k);
				} else {
					g_climbFromMs = 0;
				}
			}
			if (Finite(a.pe) && std::isfinite(a.delta)) {
				Puppet::SetClimbAnchor(a);
			}
		}

		// "Descend" at a ladder's top: Elden Ring starts the descent only from where its own capsule stands on the landing, about
		// [Ladder] top_stand m along the ladder's forward from its axis, facing the hole (back along it; live, the prompt shows only
		// that way). Snake's
		// body can stand closer, on the ladder's top between its posts: live 2026-10-31 (Stormveil, AEG219_067) he did, the puppet
		// stepped aside, Elden Ring's capsule rose 0.33 m onto the posts, the prompt went and the press was refused (twice); from
		// Elden Ring's own spot, facing the hole, the same press went down at once. So the Tarnished is put there first.
		void AlignForDescent(const Frame& a_f)
		{
			const auto& cfg = Cfg();
			if (cfg.ladderTopStand <= 0.0f || !a_f.player) {
				return;
			}
			const Vec3 at = game::Position(a_f.player);
			const GeomProbe::Asset* best = nullptr;
			float                   bestD = 2.5f;
			const auto              list = GeomProbe::Ladders(at, 60.0f);
			for (const auto& l : list) {
				const float d = LengthXZ(l.pos - at);
				if (d < bestD && l.pos.y < at.y - 1.0f && LengthXZ(l.fwd) > 0.5f) {  // a ladder going down from here
					best = &l;
					bestD = d;
				}
			}
			if (!best) {
				Log::Info("ladder: \"Descend\" -- no ladder asset found going down within 2.5 m ({} ladders loaded near): the Tarnished stays where Snake stands",
					list.size());
				return;
			}
			const Vec3 f = Normalize(Vec3{ best->fwd.x, 0.0f, best->fwd.z });
			Vec3       p{ best->pos.x + f.x * cfg.ladderTopStand, at.y + 0.6f, best->pos.z + f.z * cfg.ladderTopStand };
			Vec3       hit;
			if (!game::CastRay(p, Vec3{ 0.0f, -1.4f, 0.0f }, hit, a_f.player.chr, cfg.rayFilter) || std::fabs(hit.y - at.y) > 0.6f) {
				Log::Info("ladder: \"Descend\" -- no landing {:.2f} m along {} from its axis (the ground there: {}): the Tarnished stays where Snake stands",
					cfg.ladderTopStand, best->name, game::CastRay(p, Vec3{ 0.0f, -1.4f, 0.0f }, hit, a_f.player.chr, cfg.rayFilter) ? std::format("{:+.2f} m", hit.y - at.y) : "none");
				return;
			}
			p.y = hit.y;
			Axes axes;
			axes.mirrorX = cfg.mirrorX;
			axes.mgsvYawSign = cfg.mgsvYawSign;
			axes.erForward = Calibration::ErForward();
			game::SetPosition(a_f.player, p);
			game::SetYaw(a_f.player, ErYawForHeading(Heading(f * -1.0f), axes));  // facing the hole (the ladder's back: he turns round on it)
			Log::Info("ladder: \"Descend\" -- the Tarnished put where Elden Ring starts it: {:.2f} m from Snake, {:.2f} m along {}'s forward, facing the hole",
				Length(p - at), cfg.ladderTopStand, best->name);
		}

		void UpdateLadder(const Frame& a_f)
		{
			const auto state = a_f.player ? game::LadderState(a_f.player) : std::nullopt;
			const bool press = g_ladderPress;
			g_ladderPress = false;
			// Decided here, before this frame's prompt press is routed (SetPrompt) and the next frame's press is read.
			g_ladderUsable = state.has_value() && !Cfg().ladderPrompts.empty();
			if (!state) {
				if (a_f.player && a_f.inGame && !g_ladderUnknownLogged) {
					g_ladderUnknownLogged = true;
					Log::Warn("ladders: the player's ladder module is not the verified CS::CSPlayerLadderModule -- the ladder hand-over is off "
					          "(Tarnished mode still climbs them)");
				}
				if (g_ladder.Holding()) {
					g_ladder.Reset();
					Puppet::SetHold(Puppet::Hold::kLadder, false);
					SendClimb(a_f, g_climbPace.End(), -1);
				}
				g_climbVisual = false;
				Puppet::SetClimbVisual(false);
				return;
			}
			const int prevState = g_ladderState;
			if (*state != g_ladderState) {
				Log::Info("ER ladder state {} -> {} ({})", g_ladderState, *state, LadderStateName(*state));
				g_ladderState = *state;
				// The stick that climbed must not run Snake on (off the landing, down the shaft): it is latched as Elden Ring's climb
				// ends (EndTop / EndBottom) if it is still pushed then, until let go. Latched only at the hand-over's end (0.6.0), a
				// stick let go at the top and pushed again during the settle was taken for the climbing one and did nothing.
				if (Cfg().ladderEndEarly && g_ladder.Holding() && (*state == 6 || *state == 7) && !g_stickLatchedAtEnd) {
					g_stickLatchedAtEnd = true;
					InputBridge::HoldLeftStickUntilReleased();
				}
			}
			if (Cfg().ladderPrompts.empty()) {
				return;
			}
			LadderHandOver::Input in;
			in.nowMs = ::GetTickCount64();
			in.pressed = press;
			in.onLadder = *state != -1;
			in.snake = Puppet::Active();
			in.inWorld = a_f.inGame && a_f.player && !a_f.dead && !a_f.dying;
			in.waitMs = static_cast<std::uint64_t>(std::max(0, Cfg().ladderWaitMs));
			in.settleMs = static_cast<std::uint64_t>(std::max(0, Cfg().ladderSettleMs));
			const auto event = g_ladder.Update(in);
			if (g_ladder.PressDue(in.nowMs, static_cast<std::uint64_t>(std::max(0, Cfg().ladderPressDelayMs)))) {
				// The puppet has stepped aside: Elden Ring gets Snake's press now, as the Tarnished's own (InputBridge kept it).
				InputBridge::PressForEr(150);
				Log::Info("ladder: the press goes to Elden Ring now, {} ms after it, the Tarnished in its hands", in.nowMs - g_ladderSince);
			}
			switch (event) {
			case LadderHandOver::Event::kStarted: {
				g_stickLatchedAtEnd = false;
				g_ladderHpFrac = HpFrac(a_f);
				g_ladderSince = in.nowMs;
				g_ladderFrom = game::Position(a_f.player);
				g_climbPace.Reset();
				g_guestClimbSeen = g_guestClimbLate = false;
				g_guestClimbWasOff = !(a_f.guestReady && (a_f.guest.flags & proto::kGuestClimbing));
				g_climbSentAt = 0;
				if (press && g_ladderPrompt == Cfg().ladderDescendPrompt) {
					AlignForDescent(a_f);  // before the puppet lets go of him: Elden Ring's own place for the descent
				}
				Puppet::SetHold(Puppet::Hold::kLadder, true);
				const bool snake = Cfg().ladderSnake != "off";
				Hud::Notify(snake ? "Ladder: left stick climbs" : "Ladder: the Tarnished climbs (left stick) -- Snake is back at the end", Hud::Tone::kInfo);
				Log::Info("ladder: {} -> the puppet steps aside, Elden Ring climbs with the {} (the whole pad); HP bar {:.0f}%",
					press ? std::format("Snake's Y on its prompt (ActionButtonParam {})", g_ladderPrompt) :
							std::format("Elden Ring's ladder state {} ({}) while Snake played", *state, LadderStateName(*state)),
					snake ? std::format("hidden Tarnished, Snake drawn on the ladder ([Ladder] snake = {})", Cfg().ladderSnake) : std::string("Tarnished"),
					g_ladderHpFrac * 100.0f);
				break;
			}
			case LadderHandOver::Event::kClimbing:
				Log::Info("ladder: the climb started ({} ms after the press)", in.nowMs - g_ladderSince);
				break;
			case LadderHandOver::Event::kEnded: {
				++g_ladderClimbs;
				const Vec3 at = game::Position(a_f.player);
				// In Tarnished mode (the chord on the ladder) its own toggle carries the HP over, counted from the ladder.
				const std::string carried = g_tarnished ? std::string("Tarnished mode carries it") : CarryHp(a_f, g_ladderHpFrac);
				Puppet::SetHold(Puppet::Hold::kLadder, false);
				if (!g_stickLatchedAtEnd) {
					InputBridge::HoldLeftStickUntilReleased();  // the stick that climbed must not run Snake on (off the landing, down the shaft)
				}
				g_stickLatchedAtEnd = false;
				Log::Info("ladder: the Tarnished is off it after {:.1f} s, {:+.1f} m up, {:.1f} m away -> {}; {}; {}{} (climb #{})",
					static_cast<double>(in.nowMs - g_ladderSince) / 1000.0, at.y - g_ladderFrom.y, LengthXZ(at - g_ladderFrom),
					g_tarnished ? "Tarnished mode keeps him" : "Snake takes over where he stands", carried,
					g_climbVisual ? std::format("Snake was drawn on it (MGSV {} the climb)", g_guestClimbSeen ? "played" : "never played")
					              : std::string("the Tarnished showed"),
					g_climbEndedEarly ? std::format("; MGSV told {} ms ago, {}", in.nowMs - g_climbEndedEarlyMs,
											(a_f.guestReady && (a_f.guest.flags & proto::kGuestClimbing)) ? "Snake not home yet" : "Snake home")
					                  : std::string(),
					g_ladderClimbs);
				g_climbEndedEarly = false;
				SendClimb(a_f, g_climbPace.End(), -1);
				break;
			}
			case LadderHandOver::Event::kNoClimb:
				g_stickLatchedAtEnd = false;
				++g_ladderNoClimbs;
				Puppet::SetHold(Puppet::Hold::kLadder, false);
				SendClimb(a_f, g_climbPace.End(), -1);
				Log::Info("ladder: no climb {} ms after the press (Elden Ring refused it) -> Snake plays on", in.waitMs);
				break;
			case LadderHandOver::Event::kAborted:
				g_stickLatchedAtEnd = false;
				Puppet::SetHold(Puppet::Hold::kLadder, false);
				SendClimb(a_f, g_climbPace.End(), -1);
				Log::Info("ladder: {} on the way -> the hand-over ends", a_f.dead || a_f.dying ? "a death" : "a load");
				break;
			case LadderHandOver::Event::kNone:
				break;
			}
			// Elden Ring's climb is over for good (none right after EndTop / EndBottom, not a frame of none between two of its states):
			// MGSV is told now, not after the settle, so that it takes Snake home from its own ladder (0.18-0.23 s live) while the
			// settle runs -- the puppet then starts with Snake home and the stick moves him at once. Before (0.6.0), kHEvClimb a = 0
			// went out with the puppet's start and the stick did nothing for 0.2-0.5 s while MGSV still had him on its ladder.
			if (Cfg().ladderEndEarly && g_ladder.Current() == LadderHandOver::State::kSettling && LadderEndedForGood(prevState, *state) && !g_climbEndedEarly &&
				g_climbPace.Started()) {
				g_climbEndedEarly = true;
				g_climbEndedEarlyMs = in.nowMs;
				Log::Info("ladder: Elden Ring's climb is over ({} -> none) -> kHEvClimb a=0 now: MGSV takes Snake home during the {} ms settle",
					LadderStateName(prevState), Cfg().ladderSettleMs);
				SendClimb(a_f, g_climbPace.End(), -1);
			}
			if (g_ladder.Current() != LadderHandOver::State::kSettling) {
				g_climbEndedEarly = false;  // back on it (or off it): the pace runs again
			}
			// On the ladder: the climb's pace for MGSV (kHEvClimb), and whether Snake is drawn on it.
			if ((g_ladder.Current() == LadderHandOver::State::kClimbing || g_ladder.Current() == LadderHandOver::State::kSettling) && !g_climbEndedEarly) {
				SendClimb(a_f, g_climbPace.Update(in.nowMs, *state, game::Position(a_f.player).y, Cfg().ladderClimbSpeed), *state);
			}
			const bool guestClimbing = a_f.guestReady && (a_f.guest.flags & proto::kGuestClimbing);
			g_guestClimbWasOff = g_guestClimbWasOff || !guestClimbing;
			// MGSV plays *this* climb: its flag rose after the hand-over began (a = 1 right after the last climb's a = 0 came while MGSV
			// still took Snake home with the flag up -- that was the last climb's, review 2026-10-31).
			if (g_ladder.Holding() && guestClimbing && g_guestClimbWasOff && !g_guestClimbSeen) {
				g_guestClimbSeen = true;
				++g_guestClimbs;
				Log::Info("ladder: MGSV plays Snake's climb (kGuestClimbing) {} ms after kHEvClimb", g_climbSentAt ? in.nowMs - g_climbSentAt : 0);
			}
			if (g_ladder.Holding() && !g_guestClimbSeen && !g_guestClimbLate && g_climbSentAt &&
				in.nowMs - g_climbSentAt > static_cast<std::uint64_t>(std::max(0, Cfg().ladderGuestWaitMs))) {
				g_guestClimbLate = true;
				Log::Warn("ladder: MGSV hasn't played Snake's climb {} ms after kHEvClimb (no kGuestClimbing) -- {}", Cfg().ladderGuestWaitMs,
					Cfg().ladderSnake == "auto" ? "the Tarnished climbs instead ([Ladder] snake = auto)" : "Snake is drawn on the ladder in the pose MGSV has");
			}
			const bool want = WantClimbVisual(a_f);
			if (want != g_climbVisual) {
				if (want && Puppet::Active()) {
					g_climbFrom = Puppet::CurrentAnchor();  // where Snake is drawn now: the start of the ease
					g_climbFromMs = ::GetTickCount64();
				}
				g_climbVisual = want;
				g_snakeClimbs += want ? 1 : 0;
				Log::Info("ladder: {}", want ? "Snake is drawn on the ladder, the Tarnished stays hidden" : "Snake is no longer drawn on the ladder");
			}
			Puppet::SetClimbVisual(want);
			if (want) {
				UpdateClimbAnchor(a_f);  // before the puppet's start/stop: Snake never jumps back to where the climb began
			}
		}

		// Elden Ring's in-game HUD ([Input] interaction_hud_state, CSMenuMan UI state 7): visible while you play, hidden
		// while you rest at a grace, talk, read, choose a revival location, or ER loads / runs a cutscene (VERIFIED live: 03
		// playing, 01 in the grace menu, which is no "blocking" menu and no learned one). Only once it has been seen
		// visible since the last load, so a player who switched ER's HUD off never sees it "hidden". Not a menu signal by
		// itself: with ER's HUD set to "auto" it may hide while you just play (UNVERIFIED which UI state that hides);
		// only an interaction started with the prompt button, and the respawn (er/Puppet.cpp), wait for it.
		bool HudHidden(const Frame& a_f)
		{
			const int i = Cfg().interactionHudState;
			if (i < 0 || !a_f.inGame) {
				g_hudSeen = g_hudSeen && a_f.inGame;
				return false;
			}
			const auto states = game::UiStates();
			if (static_cast<std::size_t>(i) >= states.size()) {
				return false;
			}
			const bool visible = (states[i] & 0x02) != 0;
			g_hudSeen = g_hudSeen || visible;
			return g_hudSeen && (states[i] & 0x01) && !visible;  // created, not visible
		}

		void UpdateInteraction(Frame& a_f, std::int32_t a_promptBefore)
		{
			const auto presses = InputBridge::PromptPresses();
			const bool pressed = presses != g_promptPresses && Puppet::Active();
			g_promptPresses = presses;
			if (pressed) {
				g_interactionPrompt = a_promptBefore;
				// "Climb" / "Descend": Elden Ring climbs the ladder with the Tarnished (UpdateLadder).
				g_ladderPress = HandOverLadder(g_interactionPrompt) || g_ladderPress;
				g_ladderPrompt = g_interactionPrompt;
			}
			g_restPress = pressed && IsRestPrompt(g_interactionPrompt);
			// The HUD tells when an interaction ends only if it was up when it started (with ER's HUD on "auto" it may
			// already be hidden while you walk up to an item: then the minimum time alone).
			if (pressed) {
				g_hudUpAtPress = !a_f.hudHidden;
			}
			// An Elden Ring dialog that comes up by itself next to an NPC: live (the Castleward Tunnel grace) Melina talked after the
			// rest, then asked "Go to the Roundtable Hold" (A/B) while Snake had the pad, and nothing could answer her. Her HUD
			// hid the moment her talk prompt went away, with no press: that is an interaction as well, until the HUD is back.
			const auto nowMs = ::GetTickCount64();
			bool       dialog = !pressed && Puppet::Active() && a_f.hudHidden && !g_hudWasHidden && !g_interaction.Active() && !a_f.menuOpen && !a_f.cutscene &&
			                    g_promptSeenAt && nowMs - g_promptSeenAt < kDialogPromptMs;
			if (a_f.prompt >= 0) {
				g_promptSeenAt = nowMs;
				g_promptSeen = a_f.prompt;
			}
			if (dialog) {
				g_interactionPrompt = g_promptSeen;
				g_hudUpAtPress = true;
				Log::Info("ER dialog: its HUD hid as the prompt (ActionButtonParam {}) went, with no press (an NPC talks to the Tarnished)", g_promptSeen);
			}
			// One of Elden Ring's screens right after another (live: Melina's question answered, her grace menu 133 ms later with
			// the HUD back in between -- the interaction had ended and Snake had the pad inside the grace menu): the HUD hiding
			// again within kChainMs of an interaction's end continues it.
			bool       chained = !pressed && !dialog && Puppet::Active() && a_f.hudHidden && !g_hudWasHidden && !g_interaction.Active() && !a_f.menuOpen &&
			                     !a_f.cutscene && g_interactionEndedAt && nowMs - g_interactionEndedAt < kChainMs;
			if (chained) {
				g_hudUpAtPress = true;
				Log::Info("ER interaction continues: its HUD hid again {} ms after (the next screen)", nowMs - g_interactionEndedAt);
			}
			if (g_atGrace) {
				const bool gone = !a_f.inGame || !a_f.player || a_f.dead || a_f.dying;
				const Vec3 p = gone ? Vec3{} : game::Position(a_f.player);
				const float d = std::hypot(p.x - g_gracePos.x, p.z - g_gracePos.z);
				if (gone || d > kGraceRadius) {
					g_atGrace = false;
					Log::Info("ER: left the grace ({})", gone ? "a load or a death" : std::format("{:.1f} m from where he rested", d));
				}
			}
			const bool graceScreenRule = !pressed && !dialog && !chained && g_atGrace && Puppet::Active() && a_f.hudHidden && !g_hudWasHidden &&
			                             !g_interaction.Active() && !a_f.menuOpen && !a_f.cutscene;
			// Elden Ring's "Auto" HUD (review): a HUD hide nothing explains, twice, turns the press-less starts off.
			const bool hid = a_f.hudHidden && !g_hudWasHidden;
			const bool explained = pressed || !Puppet::Active() || g_interaction.Active() || a_f.menuOpen || a_f.cutscene || !a_f.inGame ||
			                       a_f.loading || a_f.dead || a_f.dying || g_atGrace || (g_promptSeenAt && nowMs - g_promptSeenAt < kDialogPromptMs) ||
			                       (g_interactionEndedAt && nowMs - g_interactionEndedAt < kChainMs) ||
			                       (g_restPromptAt && nowMs - g_restPromptAt < kRestWindowMs);
			if (g_autoHud.Update({ hid, explained })) {
				Log::Warn("ER HUD: hid {} times with nothing on screen to explain it -- Elden Ring's HUD looks set to \"Auto\"; the "
				          "press-less screens (an NPC's dialog, the next screen, the grace's next screen) are off for this session: talk "
				          "and use graces with the prompt button (or set ER's HUD to \"On\")", AutoHud::kAutoHudHides);
			} else if (hid && !explained && !g_autoHud.Detected()) {
				Log::Info("ER HUD: hid with nothing on screen to explain it ({} of {}: Elden Ring's \"Auto\" HUD?)", g_autoHud.Unexplained(),
					AutoHud::kAutoHudHides);
			}
			const bool presslessOk = !g_autoHud.Detected();
			if ((dialog || chained) && !presslessOk) {
				dialog = chained = false;
			}
			const bool graceScreen = graceScreenRule && presslessOk;
			if (graceScreen) {
				g_hudUpAtPress = true;
				Log::Info("ER interaction continues: its HUD hid again {} ms after, Snake still at the grace (its next screen)",
					g_interactionEndedAt ? nowMs - g_interactionEndedAt : 0);
			}
			Interaction::Input in;
			// A ladder's press is no interaction: the puppet steps aside for the climb (UpdateLadder), and Elden Ring has the pad
			// (and the chord its Tarnished-mode gate) for as long as it lasts, not a fixed 3.5 s as a "menu".
			in.pressed = (pressed && !HandOverLadder(g_interactionPrompt)) || dialog || chained || graceScreen;
			in.hudHidden = a_f.hudHidden && g_hudUpAtPress;
			in.menuOpen = a_f.menuOpen;
			in.inWorld = a_f.inGame;
			in.dead = a_f.dead || a_f.dying;
			in.nowMs = ::GetTickCount64();
			in.minMs = static_cast<std::uint64_t>(std::max(0, Cfg().interactionMinMs));
			in.maxMs = static_cast<std::uint64_t>(std::max(0, Cfg().interactionMaxMs));
			// A press-less start whose Tarnished walks away (Elden Ring has the pad) was no screen: every one of them keeps him
			// standing (Melina's talk, the grace menu); an auto HUD's fade before the detector caught it would not.
			if (dialog || chained || graceScreen) {
				g_presslessStart = true;
				g_presslessFrom = a_f.player ? game::Position(a_f.player) : Vec3{};
			} else if (in.pressed) {
				g_presslessStart = false;
			}
			const float walked = g_presslessStart && g_interaction.Active() && a_f.player && a_f.inGame ?
			                         LengthXZ(game::Position(a_f.player) - g_presslessFrom) :
			                         0.0f;
			in.abort = walked > kGraceRadius;
			if (in.abort) {
				g_presslessStart = false;
				Log::Warn("ER interaction (no press) ends: the Tarnished walked {:.1f} m -- no screen after all (Elden Ring's \"Auto\" HUD?)", walked);
			}
			const bool was = g_interaction.Active();
			a_f.interacting = g_interaction.Update(in);
			if (a_f.interacting && !was) {
				Log::Info("ER interaction (ActionButtonParam {}): Elden Ring has the controller, Snake holds still", g_interactionPrompt);
				// The mist: Elden Ring walks the Tarnished through it itself, and refused to while the puppet held him (live,
				// Margit's fog wall: the prompt was taken, he never moved; with the puppet off it worked). Step aside, then
				// take over again where Elden Ring left him.
				if (std::ranges::find(Cfg().releasePrompts, g_interactionPrompt) != Cfg().releasePrompts.end() && Puppet::Active()) {
					Puppet::SetHold(Puppet::Hold::kStepAside, true);
					g_releaseUntil = in.nowMs + static_cast<std::uint64_t>(std::max(0, Cfg().releaseMs));
					Log::Info("puppet steps aside for {} ms: Elden Ring walks the Tarnished (ActionButtonParam {})", Cfg().releaseMs, g_interactionPrompt);
				}
			} else if (!a_f.interacting && was) {
				g_interactionEndedAt = in.nowMs;
				Log::Info("ER interaction over after {} ms ({}); Snake has the controller again", in.nowMs - g_interaction.Since(),
					g_interaction.SawHudHidden() ? "its HUD is back" : "a short one: the HUD stayed");
			}
			if (g_releaseUntil && in.nowMs >= g_releaseUntil) {
				g_releaseUntil = 0;
				Puppet::SetHold(Puppet::Hold::kStepAside, false);
				Log::Info("puppet back after Elden Ring's walk");
			}
			g_erMenuOpen = a_f.menuOpen;
			a_f.menuOpen = a_f.menuOpen || a_f.interacting;
			if (a_f.hudHidden != g_hudWasHidden) {
				g_hudWasHidden = a_f.hudHidden;
				const auto now = ::GetTickCount64();
				g_hudHideScreen = a_f.hudHidden && (a_f.menuOpen || a_f.interacting || a_f.cutscene || a_f.loading || !a_f.inGame || g_atGrace ||
				                                    (g_promptSeenAt && now - g_promptSeenAt < kScreenPromptMs) ||
				                                    (g_restPromptAt && now - g_restPromptAt < kRestWindowMs));
				Log::Info("ER HUD {} (UI state {}){}", a_f.hudHidden ? "hidden" : "back", Cfg().interactionHudState,
					a_f.hudHidden && !g_hudHideScreen ? " -- nothing on screen explains it" : "");
			}
		}

		// "In the world" = the player's ChrSetEntry says Active (load_status 2, fromsoftware-rs [V-src]).
		// If a player object exists for 15 s and that never reads 2, the offset is wrong for this
		// build: say so once and fall back to "a validated player exists".
		bool InWorld(const Frame& a_f)
		{
			// Every change of the player's load status, so a live session shows the sequence a load goes through.
			static int lastStatus = -2, changes = 0;
			if (const int status = game::LoadStatus(a_f.player); status != lastStatus) {
				if (++changes <= 40) {
					Log::Info("player ChrSetEntry load status {} -> {}", lastStatus, status);
				}
				lastStatus = status;
			}
			if (game::IsActive(a_f.player)) {
				g_seenPlayer = a_f.player.chr;
				return true;
			}
			if (g_entryUnreliable) {
				return true;
			}
			if (a_f.player.chr != g_seenPlayer) {
				g_seenPlayer = a_f.player.chr;
				g_seenSince = ::GetTickCount64();
			} else if (!g_everInGame && ::GetTickCount64() - g_seenSince > 15000) {
				g_entryUnreliable = true;
				Log::Warn("player ChrSetEntry never reports Active (load_status 2/4, now {}); treating a valid player as in the world", game::LoadStatus(a_f.player));
				return true;
			}
			return false;
		}

		void UpdateHostStatus(const Frame& a_f)
		{
			auto& link = Link::Get();
			if (a_f.inGame) {
				g_everInGame = true;
				link.SetHostStatus(proto::Status::kReady, Puppet::Active() ? "in game, Snake driving" : "in game");
			} else if (a_f.player || g_everInGame) {
				link.SetHostStatus(proto::Status::kLoading, "loading");
			} else {
				link.SetHostStatus(proto::Status::kTitle, "title / menus");
			}
		}

		// The E4/E6 signals are new to the live test: log every change so it can confirm what they mean.
		void LogStateChanges(const Frame& a_f)
		{
			static bool         modal{ false }, cutscene{ false };
			static std::int32_t prompt{ -1 };
			if (a_f.modalMenu != modal) {
				Log::Info("ER blocking menu {} (CSMenuMan+0x798)", a_f.modalMenu ? "open" : "closed");
				modal = a_f.modalMenu;
			}
			if (a_f.cutscene != cutscene) {
				Log::Info("ER cutscene {}", a_f.cutscene ? "playing" : "over");
				cutscene = a_f.cutscene;
			}
			if (a_f.prompt != prompt) {
				Log::Info("ER interaction prompt {}", a_f.prompt >= 0 ? std::format("shown (ActionButtonParam {})", a_f.prompt) : std::string("gone"));
				prompt = a_f.prompt;
			}
		}

		void LogGuestMenu(bool a_open)
		{
			static bool          open{ false };
			static std::uint32_t holds{ 0 };
			static std::uint32_t cancels{ 0 }, closes{ 0 };
			if (const auto h = InputBridge::ViewHoldsToGuest(); h != holds) {
				holds = h;
				Log::Info("input: View held {} ms+ ({}) -> MGSV Start held {} ms (its iDroid; {} so far)", Cfg().viewHoldMs,
					(InputBridge::LastButtons() & 0x0020 /* View */) ? std::format("still held after {} ms", std::max(Cfg().viewHoldMs, Cfg().idroidOpenMs))
					                                                   : std::string("let go"),
					Cfg().guestIdroidStartMs, h);
			}
			if (const auto c = InputBridge::IdroidCancels(); c != cancels) {
				cancels = c;
				Log::Info("input: Start within {} ms of View -> View+Start chord; the iDroid hold sent to MGSV is cut short ({} so far)", Cfg().chordWindowMs, c);
			}
			if (const auto c = InputBridge::GuestMenuCloses(); c != closes) {
				closes = c;
				Log::Info("input: MGSV opened its menu from the cut iDroid hold anyway -> a Start tap closes it, the chord goes on ({} so far)", c);
			}
			if (a_open != open) {
				open = a_open;
				Log::Info("MGSV menu {} (kGuestInMenu): {}", a_open ? "open" : "closed",
					a_open ? "MGSV has the whole pad, Elden Ring a neutral one" : "Snake plays; Elden Ring has Start/View again");
			}
		}

		void OnFrameBegin(float a_dt)
		{
			auto& f = g_frame;
			++f.index;
			f.dt = a_dt > 0 ? a_dt : 1.0f / 60.0f;
			auto& link = Link::Get();
			link.Tick();
			f.guestAlive = link.GuestAlive();

			f.player = game::MainPlayer();
			f.inGame = f.player && InWorld(f);
			f.dead = f.player && game::IsDead(f.player);
			f.loading = !f.inGame;

			MenuWatch::Tick();
			const auto modal = game::ModalMenuOpen();
			const auto cutscene = game::CutscenePlaying();
			f.modalMenu = modal.value_or(false);
			f.cutscene = f.inGame && cutscene.value_or(false);
			f.menuOpen = MenuWatch::Open() || f.modalMenu;
			const auto promptBefore = f.prompt;
			f.prompt = f.inGame && !f.menuOpen ? game::ActionPromptParam() : -1;
			f.dying = Puppet::Dying();
			f.hudHidden = HudHidden(f);
			f.hudKnown = g_hudSeen;
			UpdateInteraction(f, f.prompt >= 0 ? f.prompt : promptBefore);
			UpdateRest(f);
			UpdateLadder(f);
			LogStateChanges(f);

			f.guestReady = false;
			f.guestRead = f.guestAlive && link.ReadGuestState(f.guest);
			if (f.guestRead) {
				// Both the per-frame flags and the boot status: a state left by a previous MGSV process never counts.
				f.guestReady = (f.guest.flags & proto::kGuestReady) && !(f.guest.flags & proto::kGuestLoading) && link.GuestStatus() == proto::Status::kReady;
			}

			PacingProbe::OnFrameBegin(f);
			Pacing::OnFrameBegin(f);  // this frame's moment on MGSV's timeline, before anything moves the puppet
			WorldShift::Check();      // an origin shift since 160 moves our grenades/mines before they fly or scan
			Console::Poll(f);
			Events::Poll(f);
			UpdateTarnished(f);
			Puppet::Decide(f);
			Progression::Tick(f);
			QuickItem::Tick(f);  // View + X: the hidden Tarnished's quick item (a flask) while Snake plays

			// While puppeting ER only gets the allow-list; MGSV gets the pad unless an ER menu (or a load,
			// or death) owns it. Not puppeting: ER plays as usual and MGSV stands still.
			const bool puppeting = Puppet::Active();
			const bool erOwnsPad = f.menuOpen || f.cutscene;
			// MGSV's own menu (iDroid, pause: kGuestInMenu) owns the whole pad; Elden Ring gets nothing until it closes. An
			// Elden Ring menu already open keeps the pad (MGSV had a neutral pad then and can't have opened one).
			const bool guestMenu = puppeting && !erOwnsPad && f.guestReady && (f.guest.flags & proto::kGuestInMenu) && Cfg().guestMenuOwnsPad;
			InputBridge::SetGuestMenu(guestMenu);
			// An interaction whose HUD hasn't hidden: Elden Ring gets buttons only (its own menus, opened meanwhile, everything).
			InputBridge::SetButtonsOnly(puppeting && f.interacting && g_interaction.ButtonsOnly() && !g_erMenuOpen && !f.cutscene);
			InputBridge::SetMode(puppeting && !erOwnsPad, !puppeting || erOwnsPad || f.loading || f.dead || f.dying);
			// A ladder's prompt: the press is deferred (er/Ladder.h): Elden Ring gets it from UpdateLadder once the puppet stepped aside.
			InputBridge::SetPrompt(puppeting && !erOwnsPad && !guestMenu && f.prompt >= 0, HandOverLadder(f.prompt));
			// Start/View and the Tarnished-mode chord (er/Chord.h): Snake's, the Tarnished's (not at a grace or in a menu: Snake
			// must never take over under one), MGSV's menu's, or Elden Ring's as they are.
			auto padMode = PadGate::Mode::kOff;
			if (guestMenu) {
				padMode = PadGate::Mode::kGuestMenu;
			} else if (puppeting && !erOwnsPad) {
				padMode = PadGate::Mode::kSnake;
			} else if ((g_tarnished || g_ladder.Holding()) && !puppeting && f.inGame && !f.menuOpen && !f.cutscene && !f.dead && !f.dying &&
			           !(g_autoHud.Detected() ? g_atGrace : f.hudHidden && g_hudHideScreen)) {
				// (With Elden Ring's "Auto" HUD its hiding says nothing: at a grace is what keeps the chord from giving Snake back there;
				// otherwise only a hide one of its screens explains keeps it.)
				// On a ladder the Tarnished has the pad as in Tarnished mode: View+Start is the chord (it keeps him after the ladder),
				// never Elden Ring's menu and map together (live: the chord on a ladder opened its menu).
				padMode = PadGate::Mode::kTarnished;
			}
			InputBridge::SetPadMode(padMode);
			LogGuestMenu(guestMenu);
			InputBridge::PollIfIdle();  // test automation without a controller (console `press`)
			Kbm::Tick();                // [Keyboard]: keys and mouse as Snake's pad (er/Kbm.h), in the modes just set
			BuddyOrders::Tick(f);
			KeyGuide::Tick(puppeting);

			link.AcquireDraw();
			if (PacingProbe::Active()) {
				const auto* list = link.FrontDraw();
				PacingProbe::OnDrawAcquired(f.index, list ? list->frame : 0);
			}
			Hud::Tick(f);
			UpdateHostStatus(f);
		}

		void OnPrePhysics(float a_dt)
		{
			g_frame.dt = a_dt > 0 ? a_dt : g_frame.dt;
			WorldShift::CheckBeforePhysics();  // Elden Ring's origin shifts land after group 11 (live): before the puppet moves
			Puppet::Move(g_frame);
		}

		void OnCamera(float)
		{
			Pacing::Resample(g_frame);
			Puppet::SyncAnchor(g_frame);
			UpdateClimbAnchor(g_frame);
			CameraDriver::Apply(g_frame);
		}

		void OnPostPhysics(float a_dt)
		{
			auto& f = g_frame;
			Pacing::Resample(f);
			Puppet::AfterPhysics(f);
			UpdateClimbAnchor(f);
			if (!Puppet::Active() && f.inGame && !f.dead && f.player) {
				Calibration::Observe(f.player, a_dt > 0 ? a_dt : f.dt);
			}
			Actors::Publish(f);
			Buddy::Update(f);
			DebugDraw::Draw(f);

			proto::HostState s{};
			auto bit = [](bool a_on, std::uint32_t a_flag) { return a_on ? a_flag : 0u; };
			s.flags = bit(f.inGame, proto::kHostInGame) | bit(f.menuOpen, proto::kHostMenuOpen) | bit(f.loading, proto::kHostLoading) |
			          bit(Puppet::Active(), proto::kHostPuppeting) | bit(f.dead || Puppet::Dying(), proto::kHostPlayerDead) | bit(f.cutscene, proto::kHostCutscene);
			s.frame = f.index;
			s.qpc = Link::Qpc();
			s.timeOfDayHours = game::TimeOfDay();
			s.cameraMaxDistance = CameraDriver::MaxDistance();
			s.erHpFrac = f.player ? (Puppet::Active() ? Puppet::HpFrac() : static_cast<float>(game::Hp(f.player)) / static_cast<float>(std::max(1, game::MaxHp(f.player)))) : 0.0f;
			// PlayerIns+0x6D0: current block (map) id [V-src CT]
			s.erWorldId = f.player ? SafeRead<std::uint32_t>(f.player.chr + 0x6D0).value_or(0) : 0;
			s.groundY = Puppet::GroundY();
			s.fallSpeed = Puppet::FallSpeed();
			Link::Get().WriteHostState(s);
		}
	}

	bool Start()
	{
		bool ok = RegisterTask(TaskGroup::kFrameBegin, &OnFrameBegin, "FrameBegin");
		if (!ok) {
			return false;
		}
		ok &= RegisterTask(TaskGroup::kChrInsPrePhysics, &OnPrePhysics, "ChrIns_PrePhysics");
		const int camGroup = Cfg().cameraTaskGroup;
		if (camGroup >= 0 && camGroup <= 168) {
			ok &= RegisterTask(static_cast<TaskGroup>(camGroup), &OnCamera, "Camera");
		} else {
			Log::Error("camera: task_group {} out of range; camera off", camGroup);
		}
		ok &= RegisterTask(TaskGroup::kWorldChrManPostPhysics, &OnPostPhysics, "WorldChrMan_PostPhysics");
		// ER's HUD compass is computed at GameFlowInGame_MoveMap_PostPhysics_1_Core0 (123), from a copy of chr_cam that ER
		// has refreshed from its own follow camera since our DrawParamUpdate write (er/CameraDriver.h).
		if (Cfg().camera && Cfg().syncCompassCam) {
			ok &= RegisterTask(TaskGroup::kDmgManPost, [](float) { CameraDriver::ApplyCompass(); }, "Compass");
		}
		WorldShift::Start();
		Renderer::Start();
		std::string why;
		if (!Puppet::Usable(&why)) {
			Log::Warn("puppet unavailable: {} (Elden Ring keeps its own control)", why);
		}
		return ok;
	}

	const Frame& Current() { return g_frame; }

	bool Tarnished() { return g_tarnished; }

	std::string TarnishedCommand(std::string_view a_arg)
	{
		// tarnished [on|off|toggle]: Tarnished mode as the chord sets it (test automation)
		const bool want = a_arg == "on" ? true : a_arg == "off" ? false : a_arg == "toggle" ? !g_tarnished : g_tarnished;
		if (want && !g_tarnished && !Puppet::Active()) {
			return "Snake is not playing: Tarnished mode needs him in control first";
		}
		SetTarnished(g_frame, want, "console");
		return std::format("Tarnished mode {} ({} toggles, {} chords; rests {}); {}", g_tarnished ? "ON" : "off", g_tarnishedToggles, g_chordsSeen, g_rests,
			Puppet::Describe());
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		// Live 2026-10-31, Fort Haight's 19 m tower: Elden Ring moved its origin 32 m down mid-climb and the summary read "-13.1 m
		// up, 8.7 m away" for a climb of +18.9 m.
		g_ladderFrom += a_delta;
		g_climbFrom.pe += a_delta;
		g_climbPace.Shift(a_delta.y);  // its measured speed: one frame of a 32 m step (eased, but enough to change the pace sent)
	}

	std::string LadderLine()
	{
		return std::format("ladder {} (ER state {} {}; {} climbs, {} without a climb); Snake drawn on {} climbs{}, MGSV played {}; kHEvClimb {} events, "
		                   "pace {:.2f} m/s; the Tarnished {}",
			Name(g_ladder.Current()), g_ladderState, LadderStateName(g_ladderState), g_ladderClimbs, g_ladderNoClimbs, g_snakeClimbs,
			g_climbVisual ? " (now)" : "", g_guestClimbs, g_climbPace.Sent(), g_climbPace.Speed(),
			Puppet::Active() || Puppet::ClimbVisual() ? "hidden" : "shown");
	}

	namespace
	{
		const char* MountedName(game::Mounted a_on)
		{
			switch (a_on) {
			case game::Mounted::kNone: return "on foot";
			case game::Mounted::kTorrent: return "on Torrent";
			case game::Mounted::kLadder: return "on a ladder";
			default: return "on ? (modules not verified)";
			}
		}
	}

	std::string StatusLine()
	{
		const auto& f = g_frame;
		auto&       link = Link::Get();
		return std::format(
			"frame {} dt {:.4f} | player {} inGame {} dead {} dying {} menu {} (blocking {}) cutscene {} prompt {} interacting {} | Tarnished mode {} "
			"({} toggles, {} chords{}; the Tarnished {}) rests {} | ladder {} (state {} {}; {} climbs, {} without a climb; Snake drawn on {}, MGSV played "
			"{}{}; kHEvClimb {}, pace {:.2f} m/s)\n"
			"guest: alive {} ready {} status {} \"{}\" hb {:.0f} ms attaches {} flags {:#x} life {}/{} buddy {}\n"
			"{}\n{}\n{}\n{}\n"
			"camera driving {} maxDist {:.2f} | input pad {} calls {} virtual pad {}{} | events {} | tasks FB {} PP {} CAM {} POST {}",
			f.index, f.dt, static_cast<void*>(f.player.chr), f.inGame, f.dead, f.dying, f.menuOpen, f.modalMenu, f.cutscene, f.prompt, f.interacting,
			g_tarnished ? "ON" : "off", g_tarnishedToggles, g_chordsSeen, InputBridge::ChordHolding() ? ", chord buttons held" : "",
			MountedName(game::PlayerMounted(f.player)), g_rests, Name(g_ladder.Current()), g_ladderState, LadderStateName(g_ladderState), g_ladderClimbs,
			g_ladderNoClimbs, g_snakeClimbs, g_guestClimbs, g_climbVisual ? ", Snake on it now" : "", g_climbPace.Sent(), g_climbPace.Speed(), f.guestAlive,
			f.guestReady,
			static_cast<int>(link.GuestStatus()), link.GuestStatusText(), link.GuestHeartbeatAgeMs(), link.AttachCount(), f.guest.flags, f.guest.life,
			f.guest.lifeMax, Buddy::TypeName(f.guest.buddyType), Puppet::Describe(), MenuWatch::Describe(), Calibration::Describe(), Buddy::Describe(),
			CameraDriver::Driving(), CameraDriver::MaxDistance(), InputBridge::PadIndex(), InputBridge::Calls(),
			InputBridge::VirtualPadExclusive() ? "exclusive" : InputBridge::VirtualPadOn() ? "shared" : "off",
			InputBridge::VirtualPadExclusive() ? std::format(" (real pad polls hidden {})", InputBridge::RealPadPollsHidden()) : std::string(), Events::Count(),
			TaskRuns(TaskGroup::kFrameBegin), TaskRuns(TaskGroup::kChrInsPrePhysics), TaskRuns(static_cast<TaskGroup>(Cfg().cameraTaskGroup)),
			TaskRuns(TaskGroup::kWorldChrManPostPhysics));
	}
}
