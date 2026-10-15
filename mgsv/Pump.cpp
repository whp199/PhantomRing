#include "Pump.h"
#include "ActorProxy.h"

#include "Addresses.h"
#include "Config.h"
#include "FultonAnchor.h"
#include "GuestScript.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Lua.h"
#include "Mapping.h"
#include "Offline.h"
#include "Pad.h"
#include "SaveGuard.h"
#include "State.h"
#include "WindowHidden.h"

#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <mutex>

namespace phantom::guest::pump
{
	namespace
	{
		using state::Boot;
		constexpr std::uint16_t kNullId = 0xFFFF;

		void*       g_gameG{ nullptr };     // the VM that has TppMain (others are ignored)
		ULONGLONG   g_lastGamePump{ 0 };
		ULONGLONG   g_lastProbe{ 0 };
		bool        g_loaded{ false };      // our script is loaded in g_gameG
		int         g_tickRef{ -1 };        // registry refs to PhantomRing.Tick / PhantomRing.Console
		int         g_consoleRef{ -1 };
		ULONGLONG   g_lastTick{ 0 };
		ULONGLONG   g_lastBoot{ 0 };
		ULONGLONG   g_lastScriptCheck{ 0 };
		std::uint64_t g_lastFrameSeen{ 0 };
		FILETIME    g_overrideTime{};
		bool        g_reloadWanted{ false };
		std::string g_scriptSource{ "none" };
		std::uint64_t g_ticks{ 0 }, g_tickErrors{ 0 };
		std::string g_lastError;

		struct BootRun
		{
			Boot        state{ Boot::kWaitLua };
			ULONGLONG   entered{ 0 };
			ULONGLONG   lastPulse{ 0 };
			ULONGLONG   lastTips{ 0 };
			ULONGLONG   requested{ 0 };
			ULONGLONG   notInGameSince{ 0 };
			bool        reloaded{ false };  // the mission itself went away (not just a pause / camera stall)
			int         tries{ 0 };
			std::string buddyStatus;
			std::string why;
			int         loadoutReloads{ 0 };  // sandbox reloads done to load [Loadout] weapons (at most one per boot)
			ULONGLONG   awaitReload{ 0 };     // a reload was asked for: don't take the still-loaded mission for the new one
			bool        boxWorn{ false };     // kBox put Snake into the box (its item menu opens on it)
			int         boxSequences{ 0 };
			ULONGLONG   boxWait{ 0 };         // kBox: next step not before this
			std::string boxStatus;
			ULONGLONG   markersClearedAt{ 0 };  // kReady: MGSV's markers last turned off ([Boot] ClearMarkersSec)
			int         proxyAutoAim{ -1 };       // kReady + [Proxies]: the auto-aim state last written (-1: not yet)
			ULONGLONG   proxyAutoAimAt{ 0 };
			ULONGLONG   buddyGoneSince{ 0 };  // kReady: the deployed buddy's object is gone (destroyed in MGSV's world)
			ULONGLONG   buddyRetryAt{ 0 };
			int         buddyRevivals{ 0 };
			ULONGLONG   buddyWhistledAt{ 0 };
			int         adoptedType{ -1 };   // MGSV's active BuddyType the guest knows as its own (-1: not seen yet)
			ULONGLONG   menuSeenAt{ 0 };     // kReady: an MGSV menu (the iDroid) was last open then
			ULONGLONG   buddyHintAt{ 0 };    // D-Dog / Quiet: their skin last looked for near Snake
			bool        noneLogged{ false }; // the buddy type went NONE without the iDroid: said once
			ULONGLONG   otherSince{ 0 };     // ...another one has been active since then
			bool        switching{ false };  // the buddy step runs for an in-session switch (Snake keeps the pad meanwhile)
			ULONGLONG   switchStarted{ 0 };
		} g_boot;

		std::mutex  g_describeLock;
		std::string g_describe{ "pump not running yet" };

		const char* MissionName(int a_code)
		{
			switch (a_code) {
			case 30050:
				return "Mother Base";
			case 30010:
				return "Afghanistan";
			case 30020:
				return "Africa";
			default:
				return "the sandbox";
			}
		}

		const char* BuddyName(const std::string& a_kind)
		{
			if (a_kind == "walker") {
				return "D-Walker";
			}
			if (a_kind == "horse") {
				return "D-Horse";
			}
			if (a_kind == "dog") {
				return "D-Dog";
			}
			if (a_kind == "quiet") {
				return "Quiet";
			}
			return "no buddy";
		}

		void Enter(Boot a_state, ULONGLONG a_now)
		{
			if (g_boot.state != a_state) {
				Log::Info("boot: {} -> {}", state::BootName(g_boot.state), state::BootName(a_state));
				if (a_state == Boot::kBox) {
					g_boot.boxWorn = false;
					g_boot.boxSequences = 0;
					g_boot.boxWait = 0;
					g_boot.boxStatus.clear();
				}
			}
			g_boot.state = a_state;
			g_boot.entered = a_now;
			state::SetBoot(a_state);
		}

		// An ini value going into a Lua string literal: TppEquip names and comma lists only.
		std::string LuaName(const std::string& a_text)
		{
			std::string out;
			for (const char ch : a_text) {
				if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ',' || ch == ' ') {
					out += ch;
				}
			}
			return out;
		}

		bool LoadoutWeapons()
		{
			const auto& c = Cfg();
			return !c.loadoutPrimary.empty() || !c.loadoutPrimary2.empty() || !c.loadoutSecondary.empty();
		}

		std::string LoadoutArgs()
		{
			const auto& c = Cfg();
			return std::format("'{}', '{}', '{}'", LuaName(c.loadoutPrimary), LuaName(c.loadoutPrimary2), LuaName(c.loadoutSecondary));
		}

		// "return PhantomRing.X(...)" -> its first result as text (or the error).
		// PhantomRing.ProxyPool, soldiers nearest the sandbox centre first (only those near Snake are realized when enabled).
		std::string ProxyPoolCall()
		{
			const auto  centre = state::Centre();
			const auto& c = Cfg();
			return centre        ? std::format("return PhantomRing.ProxyPool({}, {})", centre->x, centre->z)
			       : c.hasPosition ? std::format("return PhantomRing.ProxyPool({}, {})", c.position[0], c.position[2])
			                       : std::string("return PhantomRing.ProxyPool()");
		}

		std::string Call(const lua::Ctx& a_ctx, const std::string& a_code)
		{
			const int   top = a_ctx.Top();
			std::string error;
			if (a_ctx.Run(a_code, "=PhantomRing.boot", 1, &error) != 0) {
				return "error: " + error;
			}
			auto out = a_ctx.String(-1);
			a_ctx.SetTop(top);
			return out;
		}

		// ---- script ---------------------------------------------------------------------------------
		// The override file's timestamp; false when there is none.
		bool OverrideTime(FILETIME& a_time)
		{
			WIN32_FILE_ATTRIBUTE_DATA info{};
			if (Cfg().luaOverride.empty() || !::GetFileAttributesExW((ModuleDir() + Cfg().luaOverride).c_str(), GetFileExInfoStandard, &info)) {
				return false;
			}
			a_time = info.ftLastWriteTime;
			return true;
		}

		bool ReadOverride(std::string& a_out, FILETIME& a_time)
		{
			const auto                path = ModuleDir() + Cfg().luaOverride;
			WIN32_FILE_ATTRIBUTE_DATA info{};
			if (Cfg().luaOverride.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
				return false;
			}
			a_time = info.ftLastWriteTime;
			HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				return false;
			}
			a_out.assign(info.nFileSizeLow, '\0');
			DWORD got = 0;
			const bool ok = ::ReadFile(f, a_out.data(), info.nFileSizeLow, &got, nullptr) && got == info.nFileSizeLow;
			::CloseHandle(f);
			return ok;
		}

		void EnsureScript(const lua::Ctx& a_ctx, ULONGLONG a_now)
		{
			if (a_now - g_lastScriptCheck > 1000) {
				g_lastScriptCheck = a_now;
				FILETIME stamp{};
				if (OverrideTime(stamp) && CompareFileTime(&stamp, &g_overrideTime) != 0) {
					g_reloadWanted = true;
				}
			}
			if (g_loaded && !g_reloadWanted) {
				return;
			}
			static ULONGLONG lastAttempt = 0;
			if (!g_reloadWanted && a_now - lastAttempt < 2000) {
				return;  // a failing script is retried every 2 s, not on every pcall
			}
			lastAttempt = a_now;
			std::string override;
			FILETIME    time{};
			const bool  haveOverride = ReadOverride(override, time);
			std::string error;
			const int   top = a_ctx.Top();
			int         status = a_ctx.Run(haveOverride ? std::string_view(override) : script::Embedded(), haveOverride ? "@phantomring.lua" : "=PhantomRingGuest", 0, &error);
			g_reloadWanted = false;
			if (haveOverride) {
				g_overrideTime = time;
			}
			g_scriptSource = haveOverride ? "phantomring.lua" : "embedded";
			if (status != 0) {
				Log::Error("lua: loading the {} script failed: {}", g_scriptSource, error);
				if (!haveOverride || g_loaded) {
					return;  // keep whatever was running
				}
				// Keep running on the embedded script rather than nothing.
				status = a_ctx.Run(script::Embedded(), "=PhantomRingGuest", 0, &error);
				g_scriptSource = "embedded (override failed)";
				if (status != 0) {
					Log::Error("lua: embedded script failed too: {}", error);
					return;
				}
			}
			// Hold the entry points in the registry: no global lookups (and no metamethods) per frame, and
			// they survive scripts that reassign globals.
			if (a_ctx.Run("return PhantomRing.Tick, PhantomRing.Console", "=PhantomRing.refs", 2, &error) != 0) {
				Log::Error("lua: script loaded but its entry points are missing: {}", error);
				a_ctx.SetTop(top);
				return;
			}
			a_ctx.Unref(g_consoleRef);
			a_ctx.Unref(g_tickRef);
			g_consoleRef = a_ctx.Ref();  // pops PhantomRing.Console
			g_tickRef = a_ctx.Ref();     // pops PhantomRing.Tick
			a_ctx.SetTop(top);
			g_loaded = g_tickRef >= 0 && g_consoleRef >= 0;
			Log::Info("lua: {} script loaded (refs {} {})", g_scriptSource, g_tickRef, g_consoleRef);
		}

		// ---- console --------------------------------------------------------------------------------
		void ServeConsole(const lua::Ctx& a_ctx)
		{
			auto* c = link::GuestConsole();
			if (!c) {
				return;
			}
			const auto req = Atomic(c->reqSeq).load(std::memory_order_acquire);
			if (req == Atomic(c->respSeq).load(std::memory_order_acquire) || c->request[0] == '!') {
				return;
			}
			const std::string code(c->request, strnlen(c->request, sizeof(c->request)));
			std::string       out;
			const int         top = a_ctx.Top();
			int               status = a_ctx.Load(code, "=console", &out);
			if (status == 0) {
				// PhantomRing.Console(chunk) -> ok, text (pcall + tostring inside Lua, so nothing here can throw)
				status = a_ctx.CallRefWithValue(g_consoleRef, -1, 2, &out);
				if (status == 0) {
					out = a_ctx.String(-1);
					status = a_ctx.Boolean(-2) ? 0 : 1;
				}
			}
			a_ctx.SetTop(top);
			const auto        n = std::min(out.size(), sizeof(c->response) - 1);
			std::memcpy(c->response, out.data(), n);
			c->response[n] = 0;
			c->status = status ? 1 : 0;
			Atomic(c->respSeq).store(req, std::memory_order_release);
			Log::Debug("console: {} -> {}{}", code.substr(0, 200), status ? "error: " : "", out.substr(0, 200));
		}

		// ---- per update -----------------------------------------------------------------------------
		void Tick(const lua::Ctx& a_ctx, ULONGLONG a_now)
		{
			const auto  commands = state::TakeCommands();
			const int   top = a_ctx.Top();
			std::string error;
			if (a_ctx.CallRef(g_tickRef, commands, 11, &error) != 0) {
				if (g_tickErrors++ < 20 || error != g_lastError) {
					Log::Warn("lua: PhantomRing.Tick failed: {}", error);
				}
				g_lastError = error;
				return;
			}
			const int      first = a_ctx.Top() - 11 + 1;
			state::LuaView v;
			v.valid = true;
			v.tickMs = a_now;
			v.flags = static_cast<std::uint32_t>(a_ctx.Number(first, 0));
			v.mission = static_cast<int>(a_ctx.Number(first + 1, 0));
			if (a_ctx.Type(first + 2) == addr::kLuaTString) {
				v.sequence = a_ctx.String(first + 2);
			}
			v.buddyActive = a_ctx.Type(first + 3) == addr::kLuaTNumber ? static_cast<int>(a_ctx.Number(first + 3)) : -1;
			v.buddyId = a_ctx.Type(first + 4) == addr::kLuaTNumber ? static_cast<std::uint16_t>(a_ctx.Number(first + 4)) : kNullId;
			if (a_ctx.Type(first + 5) == addr::kLuaTNumber && a_ctx.Type(first + 6) == addr::kLuaTNumber && a_ctx.Type(first + 7) == addr::kLuaTNumber) {
				v.buddyPos = { static_cast<float>(a_ctx.Number(first + 5)), static_cast<float>(a_ctx.Number(first + 6)), static_cast<float>(a_ctx.Number(first + 7)) };
				v.buddyPosValid = v.buddyPos.Finite();
			}
			if (a_ctx.Type(first + 8) == addr::kLuaTNumber) {
				v.buddyYaw = static_cast<float>(a_ctx.Number(first + 8));
				v.buddyYawValid = std::isfinite(v.buddyYaw);
			}
			v.ridden = a_ctx.Type(first + 9) == addr::kLuaTNumber ? static_cast<std::uint16_t>(a_ctx.Number(first + 9)) : kNullId;
			// D-Horse (and D-Dog, Quiet) answer no GetPosition (live: TppHorse2 returned nothing): its own skin's root instead.
			if (!v.buddyPosValid && v.buddyId != kNullId) {
				logic::Vec3 p;
				float       yaw = 0.0f;
				if (state::BuddyPose(p, yaw)) {
					v.buddyPos = p;
					v.buddyPosValid = true;
					v.buddyYaw = yaw;
					v.buddyYawValid = true;
				}
			}
			if (!commands.empty()) {
				const auto results = a_ctx.String(first + 10);
				Log::Debug("lua: {} -> {}", commands, results);
				state::RunCommandResults(results);
			}
			a_ctx.SetTop(top);
			state::SetLua(v);
			++g_ticks;
		}

		// ---- boot -----------------------------------------------------------------------------------
		void PulseA(ULONGLONG a_now, ULONGLONG a_every)
		{
			if (Cfg().pressButtons && a_now - g_boot.lastPulse > a_every) {
				g_boot.lastPulse = a_now;
				pad::Pulse(XINPUT_GAMEPAD_A, 150);
			}
		}

		void LoadingTips(const lua::Ctx& a_ctx, ULONGLONG a_now, const state::LuaView& a_lua)
		{
			if (!(a_lua.flags & state::kLTitle) && !(a_lua.flags & state::kLPrepared) && a_now - g_boot.lastTips > 3000) {
				g_boot.lastTips = a_now;
				Log::Debug("boot: loading tips: {}", Call(a_ctx, "return PhantomRing.EndLoadingTips()"));
			}
			PulseA(a_now, 3000);
		}

		// kBox: [Loadout] BoxPoster on Snake's first cardboard box (docs/GUEST.md 18.9). Only MGSV's item menu applies a poster
		// (the var alone changes nothing on the box, and a mission load clears it): with the box worn (it is then the item the
		// menu opens on), hold D-pad left and press X as often as the cycle needs, let go, check, take the box off (RT).
		void BoxStep(const lua::Ctx& a_ctx, ULONGLONG a_now, ULONGLONG a_since)
		{
			const auto& c = Cfg();
			link::SetStatus(proto::Status::kLoading, "MGSV: putting the poster on Snake's box");
			const auto finish = [&](const char* a_why) {
				if (g_boot.boxWorn) {
					Log::Info("boot: box: {}", Call(a_ctx, "return PhantomRing.BoxItem(-1)"));
					g_boot.boxWorn = false;
				}
				Log::Info("boot: box poster: {}", a_why);
				Enter(Boot::kCentre, a_now);
			};
			if (c.boxPoster.empty()) {
				Enter(Boot::kCentre, a_now);
				return;
			}
			if (pad::SequenceRunning() || a_now < g_boot.boxWait) {
				return;
			}
			if (a_since > 30000) {
				finish("gave up after 30 s");
				return;
			}
			const auto r = Call(a_ctx, std::format("return PhantomRing.Poster('{}', '{}')", LuaName(c.boxPoster), LuaName(c.box)));
			if (r != g_boot.boxStatus) {
				Log::Info("boot: box poster {}: {}", c.boxPoster, r);
				g_boot.boxStatus = r;
			}
			if (r == "adding" || r.starts_with("box ")) {
				g_boot.boxWait = a_now + 500;  // MGSV adds the resource / the item a moment later
				return;
			}
			if (r.starts_with("done")) {
				if (g_boot.boxWorn) {
					static constexpr pad::Step kOff[] = { { 0, 255, 250 }, { 0, 0, 100 } };
					pad::Sequence(kOff);  // RT: out of the box
					Log::Info("boot: box: {}", Call(a_ctx, "return PhantomRing.BoxItem(-1)"));
					g_boot.boxWorn = false;
					g_boot.boxWait = a_now + 1500;
					return;
				}
				finish("on");
				return;
			}
			if (!r.starts_with("press ")) {
				finish(r.c_str());
				return;
			}
			int slot = -1, presses = 0;
			if (std::sscanf(r.c_str(), "press %d %d", &slot, &presses) != 2 || slot < 0 || slot > 7 || presses < 0 || presses > 8) {
				finish("bad plan");
				return;
			}
			if (!g_boot.boxWorn) {
				Log::Info("boot: box: {}", Call(a_ctx, std::format("return PhantomRing.BoxItem({})", slot)));
				g_boot.boxWorn = true;
				g_boot.boxWait = a_now + 1500;  // into the box
				return;
			}
			if (g_boot.boxSequences++ >= 3) {
				finish("the item menu didn't take it (3 tries)");
				return;
			}
			std::array<pad::Step, pad::kMaxSequence> steps{};
			std::size_t n = 0;
			steps[n++] = { XINPUT_GAMEPAD_DPAD_LEFT, 0, 900 };  // the item menu opens on the worn box
			for (int i = 0; i < presses; ++i) {
				steps[n++] = { XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_X, 0, 150 };
				steps[n++] = { XINPUT_GAMEPAD_DPAD_LEFT, 0, 350 };
			}
			steps[n++] = { XINPUT_GAMEPAD_DPAD_LEFT, 0, 300 };
			steps[n++] = { 0, 0, 700 };
			pad::Sequence(std::span(steps.data(), n));
			g_boot.boxWait = a_now + 300;
		}

		// kReady: the player deployed another buddy through MGSV's iDroid (Buddy Support > "Request deployment of D-Horse",
		// live 2026-10-13: the walker went, D-Horse came) or sent it away (Buddy Support > DISMISS D-WALKER, [Menu] BuddyDismiss;
		// live 2026-10-14: the walker is fultoned out and MGSV's active buddy type becomes NONE): follow that choice -- the buddy
		// type every buddy path keys on becomes the new one, or "none" (no revival, no follow), until the next boot, which deploys
		// the ini's again. A new buddy's place is learned anew: D-Horse is whistled over, D-Dog's and Quiet's skins are looked for
		// near Snake (they answer no GetPosition either).
		void AdoptBuddy(const lua::Ctx& a_ctx, ULONGLONG a_now, const state::LuaView& a_lua, const state::FrameView& a_fr)
		{
			auto& c = MutableCfg();
			if (state::MenuOpen()) {
				g_boot.menuSeenAt = a_now;
			}
			// D-Dog / Quiet out and their place not known: their skin near Snake (Skeleton.cpp, as D-Horse's), looked for every 5 s.
			if ((c.buddy == "dog" || c.buddy == "quiet") && a_lua.buddyActive > state::kBuddyTypeNone && a_lua.buddyId != kNullId && !a_lua.buddyPosValid &&
				a_fr.valid && a_now - g_boot.buddyHintAt > 5000) {
				g_boot.buddyHintAt = a_now;
				state::HintBuddy(a_fr.root, 12.0f, 6000);
			}
			if (a_lua.buddyActive < 0 || a_lua.buddyActive == g_boot.adoptedType) {
				g_boot.otherSince = 0;
				return;
			}
			if (!g_boot.otherSince) {
				g_boot.otherSince = a_now;
				return;
			}
			if (a_now - g_boot.otherSince < 2000) {
				return;
			}
			if (g_boot.adoptedType < 0 && c.buddy == "none") {
				// [Buddy] Type = none: what MGSV has out when the sandbox is ready stays MGSV's; a change after that is the player's.
				g_boot.adoptedType = a_lua.buddyActive;
				g_boot.otherSince = 0;
				return;
			}
			if (a_lua.buddyActive == state::kBuddyTypeNone) {
				g_boot.otherSince = 0;
				const bool fromMenu = g_boot.menuSeenAt && a_now - g_boot.menuSeenAt < 15000;
				if (c.buddy != "none" && (!c.buddyDismiss || !fromMenu)) {
					// Not the iDroid's DISMISS (or it isn't allowed): MGSV lost it by itself -- ReviveBuddy calls it back.
					if (!g_boot.noneLogged) {
						g_boot.noneLogged = true;
						Log::Info("boot: MGSV has no buddy out any more ({} expected, no iDroid DISMISS seen) -- calling it back", BuddyName(c.buddy));
					}
					return;
				}
				g_boot.adoptedType = state::kBuddyTypeNone;
				g_boot.noneLogged = false;
				if (c.buddy != "none") {
					Log::Info("boot: the player dismissed {} from the iDroid -- no buddy until another is deployed", BuddyName(c.buddy));
					c.buddy = "none";
					state::ForgetBuddy("dismissed (iDroid)");
					state::SetBuddyMissing(false);
					g_boot.buddyStatus = "dismissed (iDroid)";
					g_boot.buddyGoneSince = 0;
				}
				return;
			}
			// Asked once it has been active 2 s (right after the change MGSV can still report it half-loaded: live, nil).
			const auto kind = Call(a_ctx, "return PhantomRing.ActiveBuddyKind()");
			if (kind == c.buddy) {
				g_boot.adoptedType = a_lua.buddyActive;  // the configured one: nothing to adopt
				g_boot.otherSince = 0;
				return;
			}
			if (kind != "walker" && kind != "horse" && kind != "dog" && kind != "quiet") {
				g_boot.otherSince = a_now;  // not known (yet): asked again in 2 s
				return;
			}
			g_boot.otherSince = 0;
			g_boot.adoptedType = a_lua.buddyActive;
			g_boot.noneLogged = false;
			Log::Info("boot: the player deployed {} from the iDroid (was {}) -- following that choice", BuddyName(kind), BuddyName(c.buddy));
			c.buddy = kind;
			state::ForgetBuddy("another buddy (iDroid)");
			state::SetBuddyMissing(false);
			g_boot.buddyStatus = "active (iDroid)";
			g_boot.buddyGoneSince = 0;
			if (kind == "horse") {
				state::WhistleHorse("deployed from the iDroid");  // it gallops over; its skin is learned beside Snake
			} else if ((kind == "dog" || kind == "quiet") && a_fr.valid) {
				g_boot.buddyHintAt = a_now;
				state::HintBuddy(a_fr.root, 12.0f, 20000);  // dropped beside Snake: its skin there
			}
		}

		// kReady: the deployed buddy's object went away while Snake plays -- MGSV destroyed it in its own world (live: a rocket fired
		// with D-Walker in MGSV's line of fire; `buddy: D-Walker gone`, then `D-Walker: missing: dead` at the next boot). Elden Ring
		// has no repair, so after 10 s it is revived and called next to Snake ([Buddy] Revive), retried every second for a minute.
		void ReviveBuddy(const lua::Ctx& a_ctx, ULONGLONG a_now, const state::LuaView& a_lua, const state::FrameView& a_fr)
		{
			const auto& c = Cfg();
			if (c.buddy == "none" || !c.buddyRevive || !a_fr.valid) {
				return;
			}
			if (a_lua.buddyActive >= 0 && a_lua.buddyId != 0xFFFF) {
				if (g_boot.buddyGoneSince && a_now - g_boot.buddyGoneSince > 10000) {
					Log::Info("boot: {} back next to Snake ({})", BuddyName(c.buddy), g_boot.buddyStatus);
				}
				g_boot.buddyGoneSince = 0;
				return;
			}
			if (!g_boot.buddyGoneSince) {
				g_boot.buddyGoneSince = a_now;
				return;
			}
			const auto gone = a_now - g_boot.buddyGoneSince;
			if (gone < 10000 || a_now < g_boot.buddyRetryAt) {
				return;
			}
			g_boot.buddyRetryAt = a_now + (gone > 70000 ? 60000 : 1000);
			const float yaw = a_fr.yaw;
			const float x = a_fr.root.x + std::sin(yaw) * c.buddyAhead - std::cos(yaw) * c.buddySide;
			const float z = a_fr.root.z + std::cos(yaw) * c.buddyAhead + std::sin(yaw) * c.buddySide;
			const auto  r = Call(a_ctx, std::format("return PhantomRing.Buddy('{}', {}, {}, {}, {}, true)", c.buddy, x, a_fr.root.y, z, yaw));
			if (r != g_boot.buddyStatus) {
				Log::Info("boot: {} lost {:.0f} s ago -- {}", BuddyName(c.buddy), static_cast<double>(gone) / 1000.0, r);
				g_boot.buddyStatus = r;
				g_boot.buddyRevivals += r.starts_with("called") ? 1 : 0;
			}
			state::SetBuddyMissing(r.starts_with("missing"));
		}

		void BootStep(const lua::Ctx& a_ctx, ULONGLONG a_now)
		{
			const auto& c = Cfg();
			const auto  lua = state::Lua();
			const auto  fr = state::Frame();
			const bool  title = lua.valid && (lua.flags & state::kLTitle);
			const bool  inGame = lua.InGame() && fr.valid && a_now - fr.tickMs < 1000;
			const auto  since = a_now - g_boot.entered;
			const char* place = MissionName(c.autoBoot ? c.mission : lua.mission);
			const auto  readyText = [&] {
				const bool buddyOk = c.buddy == "none" || g_boot.buddyStatus.starts_with("active");
				return c.buddy == "none" ? std::format("MGSV: ready ({})", place)
				       : buddyOk         ? std::format("MGSV: ready ({}, {} deployed)", place, BuddyName(c.buddy))
				                         : std::format("MGSV: ready ({}; {}: {})", place, BuddyName(c.buddy), g_boot.buddyStatus);
			};

			// The player's pad reaches MGSV once the sandbox is ready (boot pulses go through either way).
			pad::SetSource(g_boot.state == Boot::kReady || c.padDuringBoot ? pad::Source::kHost : pad::Source::kNeutral);
			if (state::TakeBootRestart()) {
				g_boot.tries = 0;
				g_boot.loadoutReloads = 0;
				Enter(Boot::kWaitLua, a_now);
			}
			if (const auto req = state::TakeSandboxRequest()) {
				auto& m = MutableCfg();
				const bool sameMission = req->mission == m.mission;
				m.mission = req->mission;
				m.hasPosition = req->hasPosition;
				std::copy_n(req->position, 3, m.position);
				m.rotYDeg = req->rotYDeg;
				m.autoBoot = true;
				Log::Info("boot: sandbox -> mission {} {}", m.mission,
					m.hasPosition ? std::format("at {}, {}, {} rot {}", m.position[0], m.position[1], m.position[2], m.rotYDeg) : std::string("where the mission puts Snake"));
				state::SetCentre(std::nullopt);
				g_boot.tries = 0;
				g_boot.requested = 0;
				// Same mission: just redo the setup (warp to the new spot, new centre); else load it once.
				Enter(sameMission && lua.InGame() ? Boot::kSetup : Boot::kWaitGame, a_now);
			}
			// The buddy's deployment step (boot, or an in-session switch while ready); true = done (deployed, missing or given up).
			const auto deployBuddy = [&](ULONGLONG a_since) {
				// Beside and ahead of Snake, facing the same way: never behind him, where the camera is (MGSV fades
				// objects near its own camera; Elden Ring's renderer doesn't, a walker there fills the screen).
				// At boot the configured sandbox spot, not the frame's root: right after the setup warp the frame can still
				// report where Snake was before it (VERIFIED live: D-Walker ended up at the mission's start point, 1.2 km away).
				const bool  spot = c.hasPosition && !g_boot.switching;
				const float yaw = spot ? c.rotYDeg * std::numbers::pi_v<float> / 180.0f : fr.yaw;
				const float rx = spot ? c.position[0] : fr.root.x;
				const float ry = spot ? c.position[1] : fr.root.y;
				const float rz = spot ? c.position[2] : fr.root.z;
				// forward = (sin yaw, 0, cos yaw); right = (-cos yaw, 0, sin yaw) (VERIFIED live: Snake facing +x has +z on
				// the screen's right).
				const float x = rx + std::sin(yaw) * c.buddyAhead - std::cos(yaw) * c.buddySide;
				const float z = rz + std::cos(yaw) * c.buddyAhead + std::sin(yaw) * c.buddySide;
				const char* who = BuddyName(c.buddy);
				auto        r = Call(a_ctx, std::format("return PhantomRing.Buddy('{}', {}, {}, {}, {}, {})", c.buddy, x, ry, z, yaw, c.buddyRevive));
				if (r.starts_with("called")) {
					state::HintBuddy({ x, ry, z }, 6.0f, 10000);  // where MGSV puts it, for its skin (no GetPosition)
				}
				if (r.starts_with("spawned") && lua.buddyPosValid) {
					r = std::format("active (its skin at {:.1f},{:.1f},{:.1f})", lua.buddyPos.x, lua.buddyPos.y, lua.buddyPos.z);
				} else if (r.starts_with("spawned") && c.buddy == "horse" && a_since > 8000 && a_now - g_boot.buddyWhistledAt > 15000) {
					// Not where it was called: MGSV's own call brings it (it gallops over; the skin is learned beside Snake).
					g_boot.buddyWhistledAt = a_now;
					Log::Info("boot: {} not seen where it was called -- whistling it over: {}", who,
						state::WhistleHorse("boot: not where it was called") ? "ok" : "refused (byte checks)");
				}
				if (r != g_boot.buddyStatus) {
					Log::Info("boot: {}: {}", who, r);
					g_boot.buddyStatus = r;
				}
				if (!g_boot.switching) {
					link::SetStatus(proto::Status::kLoading, std::format("MGSV: deploying {} ({})", who, r));
				}
				state::SetBuddyMissing(r.starts_with("missing"));
				const bool done = r.starts_with("active") || r.starts_with("missing") || r.starts_with("unavailable") || r == "none" || a_since > 60000;
				if (done && a_since > 60000 && !r.starts_with("active")) {
					Log::Warn("boot: {} not deployed after 60 s ({})", who, r);
				}
				return done;
			};
			if (const auto req = state::TakeBuddyRequest()) {
				// Another buddy in session (`!buddy`, docs/GUEST.md 8): the buddy step again, next to where Snake is now, then
				// straight back to ready (no box sequence, the centre kept).
				auto& m = MutableCfg();
				Log::Info("boot: buddy {} -> {} (in session)", m.buddy, *req);
				m.buddy = *req;
				state::ForgetBuddy("another buddy");
				state::SetBuddyMissing(false);
				g_boot.buddyStatus.clear();
				if (g_boot.state == Boot::kReady && m.buddy != "none") {
					g_boot.switching = true;
					g_boot.switchStarted = a_now;
				}
			}
			switch (g_boot.state) {
			case Boot::kWaitLua:
				link::SetStatus(proto::Status::kBooting, "MGSV: starting");
				if (lua.valid) {
					Enter(title ? Boot::kTitle : Boot::kWaitGame, a_now);
				}
				break;

			case Boot::kTitle:
				link::SetStatus(proto::Status::kTitle, "MGSV: at the title screen, pressing Continue");
				if (!title) {
					Enter(Boot::kWaitGame, a_now);
				} else {
					PulseA(a_now, 2500);
				}
				break;

			case Boot::kWaitGame:
				link::SetStatus(proto::Status::kLoading, "MGSV: loading the saved game");
				if (g_boot.awaitReload) {
					// Our own reload of the sandbox (for the loadout): wait until the old mission is gone.
					if (!lua.InGame()) {
						g_boot.awaitReload = 0;
					} else if (a_now - g_boot.awaitReload > 30000) {
						Log::Warn("boot: the sandbox reload didn't start within 30 s -- going on");
						g_boot.awaitReload = 0;
					} else {
						break;
					}
				}
				if (title) {
					Enter(Boot::kTitle, a_now);
				} else if (inGame) {
					if (c.autoBoot && lua.mission != c.mission) {
						g_boot.tries = 0;
						g_boot.requested = 0;
						Enter(Boot::kLoadSandbox, a_now);
					} else {
						Enter(Boot::kSettle, a_now);
					}
				} else if (lua.sequence == "Seq_Demo_LogInKonamiServer" && !offline::Enabled()) {
					// Online, this sequence can stop on Konami's permanent region / Terms of Use prompt:
					// never press anything there for the player.
					link::SetStatus(proto::Status::kLoading, "MGSV: at Konami's login -- if it asks for region/terms, that's yours to answer (or set [Online] Offline = true)");
				} else {
					LoadingTips(a_ctx, a_now, lua);
				}
				break;

			case Boot::kLoadSandbox:
				link::SetStatus(proto::Status::kLoading, std::format("MGSV: loading {} free roam ({})", MissionName(c.mission), c.mission));
				if (inGame && lua.mission == c.mission) {
					Enter(Boot::kSettle, a_now);
					break;
				}
				if (!g_boot.requested || a_now - g_boot.requested > 90000) {
					if (inGame || !g_boot.requested) {
						if (g_boot.tries >= 3) {
							g_boot.why = std::format("mission {} did not load after 3 tries", c.mission);
							Enter(Boot::kFailed, a_now);
							break;
						}
						// The [Loadout] weapons into the sortie loadout first: the mission loads their models (docs/GUEST.md 18.7).
						if (LoadoutWeapons()) {
							Log::Info("boot: loadout before the load: {}", Call(a_ctx, std::format("return PhantomRing.PreLoadout({})", LoadoutArgs())));
						}
						// Direct first (IH loadDirect), then the ReserveMissionClear route.
						const char* method = g_boot.tries % 2 == 0 ? "direct" : "reserve";
						const auto  r = Call(a_ctx, std::format("return PhantomRing.LoadMission({}, '{}')", c.mission, method));
						Log::Info("boot: load mission {} ({}): {}", c.mission, method, r);
						++g_boot.tries;
						g_boot.requested = a_now;
					}
				}
				if (!inGame) {
					LoadingTips(a_ctx, a_now, lua);
				}
				break;

			case Boot::kSettle:
				link::SetStatus(proto::Status::kLoading, std::format("MGSV: arriving in {}", place));
				if (!inGame) {
					g_boot.entered = a_now;  // wait for 2 s of steady gameplay
					LoadingTips(a_ctx, a_now, lua);  // mission-prepare help cards ("A: Resume Game")
				} else if (since > 2000) {
					Enter(c.autoBoot ? Boot::kSetup : Boot::kCentre, a_now);
				}
				break;

			case Boot::kSetup:
				{
					link::SetStatus(proto::Status::kLoading, "MGSV: preparing the sandbox");
					const auto pos = c.hasPosition ? std::format("{}, {}, {}", c.position[0], c.position[1], c.position[2]) : std::string("nil, nil, nil");
					Log::Info("boot: setup: {}",
						Call(a_ctx, std::format("return PhantomRing.Setup({}, {}, {}, {})", pos, c.rotYDeg, c.disableSoldiers, c.invincible)));
					Log::Info("boot: clock: {}", Call(a_ctx, std::format("return PhantomRing.Clock('{}', {})", c.clockTime, c.freezeClock)));
					// The Phantom Cigar's time-lapse (Cigar.cpp): after the clock, which it needs to know frozen or not.
					Log::Info("boot: phantom cigar: {}", Call(a_ctx, std::format("return PhantomRing.Cigar({}, {})", c.cigarTimeSkip, c.cigarClockScale)));
					Log::Info("boot: weather: {}", Call(a_ctx, std::format("return PhantomRing.Weather('{}')", c.weather)));
					const auto onOff = [](bool a_on) { return a_on ? "on" : "off"; };
					Log::Info("boot: audio (music {}, ambience {}, radio {}): {}", onOff(c.music == MusicMode::kKeep), onOff(c.ambience), onOff(c.radio),
						Call(a_ctx, std::format("return PhantomRing.Audio('{}', '{}', '{}')", onOff(c.music == MusicMode::kKeep), onOff(c.ambience), onOff(c.radio))));
					Log::Info("boot: menus: {}", Call(a_ctx, std::format("return PhantomRing.Menus({}, {}, {})", c.safePauseMenu, c.blockMissions, c.buddyDismiss)));
					// MGSV's Auto-Aim Correction ([Boot] AutoAim): never changed while MGSV could save it -- [Saves] Block alone is
					// not enough (review): the file guards may be incomplete (InstallFileGuards) or the Steam swap incomplete
					// (PollSteam kIncomplete), and the guest carries on with saves live. Both guards must really be in place.
					if (!c.autoAim) {
						const bool guarded = c.blockSaves && hidden::SavesBlocked() && saves::SteamGuarded();
						Log::Info("boot: auto-aim correction: {}", guarded ? Call(a_ctx, "return PhantomRing.AutoAim(false)") :
							!c.blockSaves ? std::string("left as it is ([Saves] Block = false: it would be saved)") :
							                std::string("left as it is (the save guards are incomplete: it could be saved)"));
					}
					fulton::Reset("sandbox setup");  // its anchor soldiers are looked for again (FultonAnchor.cpp)
					// ActorProxy v0 ([Proxies], docs/GUEST.md 20): the sandbox's soldiers and hostages, for the frame thread to place.
					if (const char* why = c.ProxiesRun() ? proxy::plan::PoolRefusal(c.disableSoldiers) : nullptr) {
						Log::Warn("boot: no proxy pool: {}", why);
					} else if (c.ProxiesRun()) {
						const auto pool = Call(a_ctx, ProxyPoolCall());
						const auto parsed = proxy::plan::ParsePool(pool);
						Log::Info("boot: proxy pool: {} soldiers, {} hostages", parsed.soldiers.size(), parsed.hostages.size());
						proxy::SetPool(parsed);
						g_boot.proxyAutoAim = -1;
					}
					// [Loadout] weapons the loaded mission doesn't have: into the sortie loadout and the sandbox once more, so MGSV
					// loads their models (a weapon given mid-mission can't be drawn). Once per boot.
					if (LoadoutWeapons()) {
						const auto missing = Call(a_ctx, std::format("return PhantomRing.LoadoutMissing({})", LoadoutArgs()));
						if (missing != "0" && g_boot.loadoutReloads == 0) {
							++g_boot.loadoutReloads;
							Log::Info("boot: {} loadout weapon(s) not in this mission: {}; reloading the sandbox: {}", missing,
								Call(a_ctx, std::format("return PhantomRing.PreLoadout({})", LoadoutArgs())),
								Call(a_ctx, std::format("return PhantomRing.LoadMission({}, 'direct')", c.mission)));
							g_boot.awaitReload = a_now;
							Enter(Boot::kWaitGame, a_now);
							break;
						}
						Log::Info("boot: loadout weapons: {}", missing == "0" ? std::string("all in the mission") : std::format("{} still missing after a reload", missing));
					}
					if (!c.loadoutSupport.empty()) {
						Log::Info("boot: loadout support: {}", Call(a_ctx, std::format("return PhantomRing.Loadout('', '', '', '{}')", LuaName(c.loadoutSupport))));
					}
					g_boot.buddyStatus.clear();
					Enter(c.buddy == "none" ? Boot::kBox : Boot::kBuddy, a_now);
					break;
				}

			case Boot::kBuddy:
				if (since >= 1500 && deployBuddy(since)) {  // 1.5 s: let the setup warp land first (it is called next to Snake)
					Enter(Boot::kBox, a_now);
				}
				break;

			case Boot::kBox:
				BoxStep(a_ctx, a_now, since);
				break;

			case Boot::kCentre:
				if (since > 1000 && inGame && fr.valid) {
					state::SetCentre(fr.root);
					Log::Info("boot: sandbox centre {:.2f}, {:.2f}, {:.2f} (radius {} m)", fr.root.x, fr.root.y, fr.root.z, c.radius);
					Log::Info("boot: audio refresh: {}", Call(a_ctx, "return PhantomRing.AudioRefresh('boot')"));
					if (c.cigarTimeSkip) {
						// The Phantom Cigar in an item slot, so the player can smoke it from the item wheel (live pass B
						// 2026-10-15: the player's save carried none).
						Log::Info("boot: phantom cigar: {}", Call(a_ctx, "return PhantomRing.CigarItem()"));
					}
					Enter(Boot::kReady, a_now);
					link::SetStatus(proto::Status::kReady, readyText());
				}
				break;

			case Boot::kReady:
				{
					if (title) {
						Enter(Boot::kTitle, a_now);
					} else if (!inGame && lua.InGame() && state::MenuOpen()) {
						// MGSV's pause menu stops the player-camera frames: still the sandbox, still ready, a menu owns the
						// pad (Frame.cpp republishes GuestState with kGuestInMenu meanwhile).
						g_boot.notInGameSince = 0;
						link::SetStatus(proto::Status::kReady, readyText());
					} else if (!inGame) {
						if (!g_boot.notInGameSince) {
							g_boot.notInGameSince = a_now;
							g_boot.reloaded = false;
						}
						// MGSV's pause menu and some camera states stop the player-camera frames for seconds while
						// the mission stays prepared (VERIFIED live 2026-10-12): only a real reload (sequence or
						// mission-prepared state gone) redoes the sandbox setup afterwards.
						g_boot.reloaded |= !lua.InGame();
						link::SetStatus(proto::Status::kLoading, "MGSV: loading");
						if (a_now - g_boot.notInGameSince > 60000) {
							Enter(Boot::kWaitGame, a_now);
						}
					} else {
						if (g_boot.notInGameSince && a_now - g_boot.notInGameSince > 2000 && g_boot.reloaded) {
							// The mission reloaded (MGSV continue/restart): soldiers, buddy and centre again.
							Log::Info("boot: gameplay resumed after a load -- redoing the sandbox setup");
							g_boot.notInGameSince = 0;
							Enter(c.autoBoot && lua.mission != c.mission ? Boot::kLoadSandbox : Boot::kSettle, a_now);
							g_boot.tries = 0;
							g_boot.requested = 0;
							break;
						}
						g_boot.notInGameSince = 0;
						if (g_boot.switching) {
							if (deployBuddy(a_now - g_boot.switchStarted)) {
								g_boot.switching = false;
								Log::Info("boot: in-session buddy switch done ({})", g_boot.buddyStatus);
								link::SetStatus(proto::Status::kReady, readyText());
							}
							break;
						}
						AdoptBuddy(a_ctx, a_now, lua, fr);
						ReviveBuddy(a_ctx, a_now, lua, fr);
						// MGSV's Fulton on a sleeping Elden Ring enemy through an anchor soldier ([Fulton], FultonAnchor.cpp, docs/GUEST.md 20.4).
						fulton::LuaStep(a_now, [&](const std::string& a_code) { return Call(a_ctx, a_code); });
						if (!proxy::TakePoolRequest()) {
						} else if (const char* why = proxy::plan::PoolRefusal(c.disableSoldiers)) {
							Log::Warn("boot: no proxy pool (asked for): {}", why);
						} else {
							const auto parsed = proxy::plan::ParsePool(Call(a_ctx, ProxyPoolCall()));
							Log::Info("boot: proxy pool (asked for): {} soldiers, {} hostages", parsed.soldiers.size(), parsed.hostages.size());
							proxy::SetPool(parsed);
							g_boot.proxyAutoAim = -1;
						}
						// ActorProxy v1: the slot soldiers' enable/puppet/sleep/disable and MGSV's Fulton on them (ActorProxyGame.cpp).
						proxy::LuaStep(a_now, [&](const std::string& a_code) { return Call(a_ctx, a_code); });
						// ActorProxy: MGSV's Auto-Aim Correction on only while every pool soldier is placed (a disabled soldier left standing
						// pulls the aim, 18.13), and only with the save guards complete (as [Boot] AutoAim). Checked once a second.
						// [Boot] AutoAim = true keeps MGSV's own setting, proxies or not.
						if (!c.proxies && g_boot.proxyAutoAim == 1 && !c.autoAim) {
							// Proxies turned off in session (`!proxy off`): the setting goes back to off as [Boot] AutoAim wants.
							Log::Info("boot: proxies off: auto-aim correction {}", Call(a_ctx, "return PhantomRing.AutoAim(false)"));
							g_boot.proxyAutoAim = 0;
						}
						if (c.proxies && c.proxyAutoAim && !c.autoAim && a_now - g_boot.proxyAutoAimAt >= 1000) {
							g_boot.proxyAutoAimAt = a_now;
							const bool guarded = c.blockSaves && hidden::SavesBlocked() && saves::SteamGuarded();
							const int  want = proxy::AutoAimWanted(guarded) ? 1 : 0;
							if (guarded && want != g_boot.proxyAutoAim) {
								Log::Info("boot: proxies: auto-aim correction {}", Call(a_ctx, want ? "return PhantomRing.AutoAim(true)" : "return PhantomRing.AutoAim(false)"));
								g_boot.proxyAutoAim = want;
							}
						}
						// Markers stay only while every pool soldier is placed: a marked soldier left at his post pulled the aim (18.13).
						const bool keepMarkers = c.proxies && c.proxyKeepMarkers && proxy::AllPlaced();
						if (!keepMarkers && c.clearMarkersSec > 0 && a_now - g_boot.markersClearedAt >= static_cast<ULONGLONG>(c.clearMarkersSec) * 1000) {
							// D-Walker's Search Mode and Snake's own looking mark MGSV soldiers again: kept off.
							g_boot.markersClearedAt = a_now;
							const auto r = Call(a_ctx, "return PhantomRing.ClearMarkers()");
							if (r != "true") {
								Log::Warn("boot: MGSV's markers could not be turned off ({}); [Boot] ClearMarkersSec", r);
							}
						}
						if (c.autoBoot && lua.mission != c.mission) {
							Log::Info("boot: MGSV left the sandbox (mission {})", lua.mission);
							g_boot.tries = 0;
							g_boot.requested = 0;
							Enter(Boot::kLoadSandbox, a_now);
							break;
						}
						link::SetStatus(proto::Status::kReady, readyText());
					}
					break;
				}

			case Boot::kFailed:
				link::SetStatus(proto::Status::kFailed, std::format("MGSV: boot failed: {} (see PhantomRingGuest.log)", g_boot.why));
				break;
			}
		}
	}

	void Run(void* a_L)
	{
		if (!a_L) {
			return;
		}
		const auto     now = ::GetTickCount64();
		const lua::Ctx ctx(a_L);
		void*          g = ctx.GlobalState();
		if (!g) {
			return;
		}
		if (g != g_gameG) {
			// Only the VM that runs the game scripts (it has TppMain) is ours; adopt a new one only when
			// the old one has gone quiet (a recreated VM). Probing compiles a chunk: at most 4x a second.
			if ((g_gameG && now - g_lastGamePump < 5000) || now - g_lastProbe < 250) {
				return;
			}
			g_lastProbe = now;
			const int top = ctx.Top();
			const bool game = ctx.Run("return type(TppMain) == 'table'", "=PhantomRing.probe", 1, nullptr) == 0 && ctx.Boolean(-1);
			ctx.SetTop(top);
			if (!game) {
				return;
			}
			Log::Info("lua: game VM {} ({})", g, g_gameG ? "replaced the previous one" : "found");
			g_gameG = g;
			g_loaded = false;  // refs of another VM mean nothing here
			g_tickRef = g_consoleRef = -1;
		}
		g_lastGamePump = now;

		EnsureScript(ctx, now);
		if (!g_loaded) {
			return;
		}
		ServeConsole(ctx);

		const auto frame = state::Frame().frame;
		if (frame != g_lastFrameSeen || now - g_lastTick >= 33) {
			g_lastFrameSeen = frame;
			g_lastTick = now;
			Tick(ctx, now);
			state::RunLuaTickWork(now, g_boot.state == Boot::kReady);
		}
		if (now - g_lastBoot >= 250) {
			g_lastBoot = now;
			BootStep(ctx, now);
			state::RunLuaThreadWork(now, g_boot.state == Boot::kReady);
			std::scoped_lock lock(g_describeLock);
			const auto       lua = state::Lua();
			g_describe = std::format("boot {} ({}) script {} ticks {} tick-errors {} lua: mission {} seq {} flags {:#x} buddy {} id {} pos-valid {} ridden {}",
				state::BootName(g_boot.state), link::CurrentStatusText(), g_scriptSource, g_ticks, g_tickErrors, lua.mission, lua.sequence, lua.flags,
				lua.buddyActive, lua.buddyId, lua.buddyPosValid, lua.ridden);
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_describeLock);
		return g_describe;
	}
}
