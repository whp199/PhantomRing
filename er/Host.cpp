#include "Host.h"

#include "Actors.h"
#include "Addresses.h"
#include "Buddy.h"
#include "BuddyOrders.h"
#include "Calibration.h"
#include "CameraDriver.h"
#include "Config.h"
#include "Console.h"
#include "DebugDraw.h"
#include "Events.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Interaction.h"
#include "KeyGuide.h"
#include "Link.h"
#include "Log.h"
#include "Mem.h"
#include "MenuWatch.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Puppet.h"
#include "Renderer.h"
#include "Tasks.h"
#include "WorldShift.h"

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
		bool          g_hudUpAtPress{ true };
		bool          g_erMenuOpen{ false };  // an Elden Ring menu of its own (not the interaction folded into menuOpen)

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
			}
			// The HUD tells when an interaction ends only if it was up when it started (with ER's HUD on "auto" it may
			// already be hidden while you walk up to an item: then the minimum time alone).
			if (pressed) {
				g_hudUpAtPress = !a_f.hudHidden;
			}
			Interaction::Input in;
			in.pressed = pressed;
			in.hudHidden = a_f.hudHidden && g_hudUpAtPress;
			in.menuOpen = a_f.menuOpen;
			in.inWorld = a_f.inGame;
			in.dead = a_f.dead || a_f.dying;
			in.nowMs = ::GetTickCount64();
			in.minMs = static_cast<std::uint64_t>(std::max(0, Cfg().interactionMinMs));
			in.maxMs = static_cast<std::uint64_t>(std::max(0, Cfg().interactionMaxMs));
			const bool was = g_interaction.Active();
			a_f.interacting = g_interaction.Update(in);
			if (a_f.interacting && !was) {
				Log::Info("ER interaction (ActionButtonParam {}): Elden Ring has the controller, Snake holds still", g_interactionPrompt);
				// The mist: Elden Ring walks the Tarnished through it itself, and refused to while the puppet held him (live,
				// Margit's fog wall: the prompt was taken, he never moved; with the puppet off it worked). Step aside, then
				// take over again where Elden Ring left him.
				if (std::ranges::find(Cfg().releasePrompts, g_interactionPrompt) != Cfg().releasePrompts.end() && Puppet::Active()) {
					Puppet::SetOverride(Puppet::Override::kOff);
					g_releaseUntil = in.nowMs + static_cast<std::uint64_t>(std::max(0, Cfg().releaseMs));
					Log::Info("puppet steps aside for {} ms: Elden Ring walks the Tarnished (ActionButtonParam {})", Cfg().releaseMs, g_interactionPrompt);
				}
			} else if (!a_f.interacting && was) {
				Log::Info("ER interaction over after {} ms ({}); Snake has the controller again", in.nowMs - g_interaction.Since(),
					g_interaction.SawHudHidden() ? "its HUD is back" : "a short one: the HUD stayed");
			}
			if (g_releaseUntil && in.nowMs >= g_releaseUntil) {
				g_releaseUntil = 0;
				Puppet::SetOverride(Puppet::Override::kAuto);
				Log::Info("puppet back after Elden Ring's walk");
			}
			g_erMenuOpen = a_f.menuOpen;
			a_f.menuOpen = a_f.menuOpen || a_f.interacting;
			if (a_f.hudHidden != g_hudWasHidden) {
				g_hudWasHidden = a_f.hudHidden;
				Log::Info("ER HUD {} (UI state {})", a_f.hudHidden ? "hidden" : "back", Cfg().interactionHudState);
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
			if (const auto h = InputBridge::ViewHoldsToGuest(); h != holds) {
				holds = h;
				Log::Info("input: View held {} ms -> MGSV Start held {} ms (its iDroid; {} so far)", Cfg().viewHoldMs, Cfg().guestIdroidStartMs, h);
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
			LogStateChanges(f);

			f.guestReady = false;
			if (f.guestAlive && link.ReadGuestState(f.guest)) {
				// Both the per-frame flags and the boot status: a state left by a previous MGSV process never counts.
				f.guestReady = (f.guest.flags & proto::kGuestReady) && !(f.guest.flags & proto::kGuestLoading) && link.GuestStatus() == proto::Status::kReady;
			}

			PacingProbe::OnFrameBegin(f);
			Pacing::OnFrameBegin(f);  // this frame's moment on MGSV's timeline, before anything moves the puppet
			WorldShift::Check();      // an origin shift since 160 moves our grenades/mines before they fly or scan
			Console::Poll(f);
			Events::Poll(f);
			Puppet::Decide(f);

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
			InputBridge::SetPrompt(puppeting && !erOwnsPad && !guestMenu && f.prompt >= 0);
			LogGuestMenu(guestMenu);
			InputBridge::PollIfIdle();  // test automation without a controller (console `press`)
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
			CameraDriver::Apply(g_frame);
		}

		void OnPostPhysics(float a_dt)
		{
			auto& f = g_frame;
			Pacing::Resample(f);
			Puppet::AfterPhysics(f);
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

	std::string StatusLine()
	{
		const auto& f = g_frame;
		auto&       link = Link::Get();
		return std::format(
			"frame {} dt {:.4f} | player {} inGame {} dead {} dying {} menu {} (blocking {}) cutscene {} prompt {} interacting {}\n"
			"guest: alive {} ready {} status {} \"{}\" hb {:.0f} ms attaches {} flags {:#x} life {}/{} buddy {}\n"
			"{}\n{}\n{}\n{}\n"
			"camera driving {} maxDist {:.2f} | input pad {} calls {} | events {} | tasks FB {} PP {} CAM {} POST {}",
			f.index, f.dt, static_cast<void*>(f.player.chr), f.inGame, f.dead, f.dying, f.menuOpen, f.modalMenu, f.cutscene, f.prompt, f.interacting, f.guestAlive,
			f.guestReady,
			static_cast<int>(link.GuestStatus()), link.GuestStatusText(), link.GuestHeartbeatAgeMs(), link.AttachCount(), f.guest.flags, f.guest.life,
			f.guest.lifeMax, Buddy::TypeName(f.guest.buddyType), Puppet::Describe(), MenuWatch::Describe(), Calibration::Describe(), Buddy::Describe(),
			CameraDriver::Driving(), CameraDriver::MaxDistance(), InputBridge::PadIndex(), InputBridge::Calls(), Events::Count(),
			TaskRuns(TaskGroup::kFrameBegin), TaskRuns(TaskGroup::kChrInsPrePhysics), TaskRuns(static_cast<TaskGroup>(Cfg().cameraTaskGroup)),
			TaskRuns(TaskGroup::kWorldChrManPostPhysics));
	}
}
