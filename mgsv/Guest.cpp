#include "Guest.h"

#include "Ammo.h"
#include "Blast.h"
#include "Camera.h"
#include "Capture.h"
#include "Combat.h"
#include "Config.h"
#include "Death.h"
#include "Follow.h"
#include "Frame.h"
#include "FramePace.h"
#include "GuestScript.h"
#include "Hook.h"
#include "Hold.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Lua.h"
#include "Menu.h"
#include "MemScan.h"
#include "Watch.h"
#include "Offline.h"
#include "Pad.h"
#include "Placed.h"
#include "Pump.h"
#include "SaveGuard.h"
#include "Sound.h"
#include "Throw.h"
#include "Skeleton.h"
#include "State.h"
#include "Version.h"
#include "Vars.h"
#include "Verify.h"

#include <xinput.h>

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>
#include "Platform.h"
#include "WindowGuard.h"
#include "WindowHidden.h"

#include <atomic>
#include <format>
#include <mutex>

namespace phantom::guest
{
	namespace
	{
		std::atomic<bool> g_active{ false };
		std::once_flag    g_lateOnce;

		bool EnvActivated()
		{
			wchar_t    value[8]{};
			const auto n = ::GetEnvironmentVariableW(L"PHANTOMRING", value, 8);
			return n == 1 && value[0] == L'1';
		}

		void DumpDefaultScript()
		{
			const auto path = ModuleDir() + L"phantomring.default.lua";
			HANDLE     f = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				return;
			}
			const auto script = script::Embedded();
			DWORD      put = 0;
			::WriteFile(f, script.data(), static_cast<DWORD>(script.size()), &put, nullptr);
			::CloseHandle(f);
		}

		// MGSV's Lua thread, every 250 ms: the buddy follow.
		void LuaWork(std::uint64_t a_nowMs, bool a_ready)
		{
			follow::Update(a_nowMs, a_ready);
		}

		// MGSV's Lua thread after every Tick (~30 Hz, frames or not): menus, the GuestState while frames stall, the walker's
		// place for the volume detour.
		void LuaTickWork(std::uint64_t a_nowMs, bool a_ready)
		{
			menu::Poll(a_nowMs);
			frame::PublishStalled(a_nowMs, a_ready);
			sound::Tick(a_ready);
		}

		std::string Command(std::string_view a_line)
		{
			const auto cmd = a_line.substr(1);
			if (cmd == "status" || cmd.empty()) {
				const auto centre = state::Centre();
				return std::format("status: {}\nboot: {}\npump: {}\nframe: {}\ndeath: {}\nfollow: {}\n{}\n{}\n{}\n{}\n{}\n{}  tarnished mode: {}\nsteam saves guarded: {}  host alive: {}  centre: {}\n", link::CurrentStatusText(),
					state::BootName(state::CurrentBoot()), pump::Describe(), frame::Describe(), death::Describe(), follow::Describe(), skeleton::DescribeBuddy(), menu::Describe(), blast::Describe(), throwing::Describe(), placed::Describe(),
					ammo::Describe(), state::Parked() ? "on (Snake parked)" : "off", saves::SteamGuarded(), link::HostAlive(), centre ? std::format("{:.2f},{:.2f},{:.2f}", centre->x, centre->y, centre->z) : std::string("none"));
			}
			if (cmd == "boot") {
				state::RequestBootRestart();
				return "boot sequence restarted";
			}
			if (cmd == "treadmill off" || cmd == "treadmill on") {
				state::SetTreadmillEnabled(cmd.ends_with("on"));
				return std::string(cmd);
			}
			if (cmd == "centre" || cmd == "center") {
				const auto f = state::Frame();
				if (!f.valid) {
					return "no player frame yet";
				}
				state::SetCentre(f.root);
				return std::format("centre {:.2f},{:.2f},{:.2f}", f.root.x, f.root.y, f.root.z);
			}
			if (cmd.starts_with("pulse")) {
				// !pulse <buttons> [ms]: buttons = a b x y start back lb rb up down left right (joined with +)
				std::string_view rest = cmd.substr(5);
				while (!rest.empty() && rest.front() == ' ') {
					rest.remove_prefix(1);
				}
				const auto   space = rest.find(' ');
				const auto   names = rest.substr(0, space);
				unsigned     ms = 150;
				if (space != std::string_view::npos) {
					ms = static_cast<unsigned>(std::strtoul(std::string(rest.substr(space + 1)).c_str(), nullptr, 10));
				}
				std::uint16_t buttons = 0;
				std::size_t   start = 0;
				while (start <= names.size()) {
					const auto end = names.find('+', start);
					const auto name = names.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
					static constexpr std::pair<std::string_view, std::uint16_t> kNames[] = { { "a", XINPUT_GAMEPAD_A }, { "b", XINPUT_GAMEPAD_B },
						{ "x", XINPUT_GAMEPAD_X }, { "y", XINPUT_GAMEPAD_Y }, { "start", XINPUT_GAMEPAD_START }, { "back", XINPUT_GAMEPAD_BACK },
						{ "lb", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER }, { "up", XINPUT_GAMEPAD_DPAD_UP },
						{ "down", XINPUT_GAMEPAD_DPAD_DOWN }, { "left", XINPUT_GAMEPAD_DPAD_LEFT }, { "right", XINPUT_GAMEPAD_DPAD_RIGHT } };
					bool known = false;
					for (const auto& [n, b] : kNames) {
						if (n == name) {
							buttons |= b;
							known = true;
						}
					}
					if (!known) {
						return std::format("unknown button '{}'", name);
					}
					if (end == std::string_view::npos) {
						break;
					}
					start = end + 1;
				}
				pad::Pulse(buttons, ms ? ms : 150);
				return std::format("pulsed {:#06x} for {} ms (pad polls so far {})", buttons, ms ? ms : 150, pad::Polls());
			}
			if (cmd.starts_with("sandbox")) {
				// !sandbox <mission> [x y z [rotYDeg]]
				std::istringstream in{ std::string(cmd.substr(7)) };
				state::SandboxRequest r;
				if (!(in >> r.mission) || r.mission <= 0) {
					return "usage: !sandbox <mission> [x y z [rotYDeg]]   (30050 Mother Base, 30010 Afghanistan, 30020 Africa)";
				}
				if (in >> r.position[0] >> r.position[1] >> r.position[2]) {
					r.hasPosition = true;
					in >> r.rotYDeg;
				}
				state::RequestSandbox(r);
				return std::format("sandbox request: mission {}{}", r.mission,
					r.hasPosition ? std::format(" at {}, {}, {} rot {}", r.position[0], r.position[1], r.position[2], r.rotYDeg) : std::string());
			}
			if (cmd.starts_with("hold")) {
				// !hold <off|treadmill|native_treadmill|native> [radius]
				std::istringstream in{ std::string(cmd.substr(4)) };
				std::string        mode;
				state::HoldRequest r;
				in >> mode >> r.radius;
				if (mode == "off") {
					r.mode = static_cast<int>(HoldMode::kOff);
				} else if (mode == "treadmill") {
					r.mode = static_cast<int>(HoldMode::kTreadmill);
				} else if (mode == "native_treadmill") {
					r.mode = static_cast<int>(HoldMode::kNativeTreadmill);
				} else if (mode == "native") {
					r.mode = static_cast<int>(HoldMode::kNative);
				} else {
					return "usage: !hold <off|treadmill|native_treadmill|native> [radius]";
				}
				state::RequestHold(r);
				return std::format("hold request: {} radius {}", mode, r.radius);
			}
			if (cmd == "buddy" || cmd.starts_with("buddy ")) {
				// !buddy [walker|horse|dog|quiet|none]: another buddy in session (Pump.cpp re-runs its deployment), or its state
				if (cmd == "buddy") {
					return std::format("buddy {}: {}\n{}", Cfg().buddy, follow::Describe(), skeleton::DescribeBuddy());
				}
				const auto type = std::string(cmd.substr(6));
				if (type != "walker" && type != "horse" && type != "dog" && type != "quiet" && type != "none") {
					return "usage: !buddy [walker|horse|dog|quiet|none]";
				}
				state::RequestBuddy(type);
				return "buddy switch requested: " + type;
			}
			if (cmd == "regen on" || cmd == "regen off") {
				// !regen on|off: MGSV's post-hit regeneration curve for Elden Ring's hits ([Combat] RegenGate), for A/B tests
				MutableCfg().regenGate = cmd.ends_with("on");
				return std::format("regen gate {}", Cfg().regenGate ? "on" : "off");
			}
			if (cmd.starts_with("loc ")) {
				// !loc <GameObject id>: the player location manager's object for another id (EXPERIMENT, D-Horse)
				return hold::DescribeLocation(static_cast<std::uint16_t>(std::strtoul(std::string(cmd.substr(4)).c_str(), nullptr, 0)));
			}
			if (cmd.starts_with("placeobj")) {
				// !placeobj <id> <x> <y> <z> [yaw rad]: that object's set-position on the next game frame (EXPERIMENT)
				std::istringstream in{ std::string(cmd.substr(8)) };
				std::string        id;
				float              x = 0, y = 0, z = 0, yaw = 0;
				if (!(in >> id >> x >> y >> z)) {
					return "usage: !placeobj <id> <x> <y> <z> [yaw]; last: " + hold::LastPlaceResult();
				}
				in >> yaw;
				hold::RequestPlaceObject(static_cast<std::uint16_t>(std::strtoul(id.c_str(), nullptr, 0)), { x, y, z }, yaw);
				return "place requested (next frame); last: " + hold::LastPlaceResult();
			}
			if (cmd == "whistle") {
				follow::RequestWhistle();
				return "whistle requested (next Lua update): " + follow::Describe();
			}
			if (cmd == "follow on" || cmd == "follow off") {
				follow::SetEnabled(cmd.ends_with("on"));
				return "follow: " + follow::Describe();
			}
			if (cmd.starts_with("follow method ")) {
				// !follow method command|restore|record: how auto-follow sends D-Walker ([Buddy] FollowMethod, docs/GUEST.md 18.3)
				if (!follow::SetMethod(std::string(cmd.substr(14)))) {
					return "usage: !follow method command|restore|record";
				}
				return "follow: " + follow::Describe();
			}
			if (cmd.starts_with("follow modes ")) {
				// !follow modes <normal stealth search intercept mode1 mode2 | all>: the walker modes auto-follow acts in
				follow::SetModes(std::string(cmd.substr(13)));
				return "follow: " + follow::Describe();
			}
			if (cmd.starts_with("follow spot ")) {
				// !follow spot <ahead m> <side m>: where whistles send D-Walker (see [Buddy] FollowAhead/FollowSide)
				std::istringstream in{ std::string(cmd.substr(12)) };
				float              ahead = 0, side = 0;
				if (!(in >> ahead >> side) || std::fabs(ahead) > 20 || std::fabs(side) > 20) {
					return "usage: !follow spot <ahead m> <side m>";
				}
				MutableCfg().buddyFollowAhead = ahead;
				MutableCfg().buddyFollowSide = side;
				return std::format("follow: spot {} m ahead, {} m right (next whistle)", ahead, side);
			}
			if (cmd.starts_with("follow ")) {
				// !follow <distance m> <every ms>: tune live (the ini sets them at start)
				std::istringstream in{ std::string(cmd.substr(7)) };
				float              dist = 0;
				int                every = 0;
				if (!(in >> dist >> every) || dist <= 0 || every < 100) {
					return "usage: !follow on|off | !follow <distance m> <every ms> | !follow method <m> | !follow modes <list>";
				}
				MutableCfg().buddyFollowDistance = dist;
				MutableCfg().buddyFollowEveryMs = every;
				return std::format("follow: distance {} m, every {} ms", dist, every);
			}
			if (cmd == "blast") {
				return blast::Describe();
			}
			if (cmd == "shots") {
				return combat::DescribeDamage();
			}
			if (cmd == "throw") {
				return throwing::Describe();
			}
			if (cmd == "placed") {
				return placed::Describe();
			}
			if (cmd.starts_with("detonate ")) {
				// Dev: what the host's kHEvDetonate does, queued for the game thread like a host event would be.
				const auto id = static_cast<std::uint32_t>(std::strtoul(std::string(cmd.substr(9)).c_str(), nullptr, 0));
				placed::RequestDetonate(id);
				return std::format("detonate #{} queued", id);
			}
			if (cmd == "menu") {
				return menu::Describe();
			}
			if (cmd == "refill") {
				// Dev: what the host's kHEvRested does (magazines, ammo stocks, items to full; docs/GUEST.md 18.11)
				ammo::Request("console");
				return "refill requested (next Lua update): " + ammo::Describe();
			}
			if (cmd == "ammo") {
				return ammo::Describe();
			}
			if (cmd == "ammo drain") {
				return ammo::Drain();
			}
			if (cmd == "sound" || cmd.starts_with("sound ")) {
				return sound::Command(std::string(cmd.size() > 5 ? cmd.substr(6) : std::string_view{}));
			}
			if (cmd == "pad") {
				return std::format("pad polls {}  offline {} (refused {} connections)", pad::Polls(), offline::Enabled(), offline::Refused());
			}
			if (std::string watchOut; watch::Command(cmd, watchOut)) {
				return watchOut;
			}
			if (std::string memOut; memscan::Command(cmd, memOut)) {
				return memOut;
			}
			if (cmd.starts_with("pace")) {
				return framepace::Command(cmd.substr(4));
			}
			if (cmd.starts_with("capture")) {
				return capture::Command(cmd.substr(7));
			}
			if (cmd == "window") {
				return hidden::Describe();
			}
			if (cmd == "debug on" || cmd == "debug off") {
				Log::SetDebug(cmd.ends_with("on"));
				return std::string(cmd);
			}
			return "guest commands: !status !boot !treadmill on|off !centre !pulse <a+b..> [ms] !pad !sandbox <mission> [x y z rot] !hold <mode> [radius] !whistle !follow ... !blast !menu !refill !ammo !sound [volume g] !capture [probe N|...] !window !debug on|off   (anything else is Lua)";
		}

		DWORD WINAPI Maintenance(void*)
		{
			// Full identity: the cheap header check let us hook from DllMain; the hash settles it.
			const auto id = CheckExe();
			const auto hash = Sha256File(id.path);
			if (hash.empty()) {
				Log::Warn("identity: could not hash {}", Narrow(id.path));
			} else if (hash != addr::kSha256) {
				Log::Error("identity: sha256 {} is not 1.0.15.4's -- disabling every hook (plain dinput8 proxy from now on)", hash);
				g_active.store(false);
				Hooks::DisableAll();
				link::SetStatus(proto::Status::kFailed, "MGSV: unsupported mgsvtpp.exe (sha256 mismatch)");
				return 0;
			} else {
				Log::Info("identity: sha256 matches mgsvtpp.exe 1.0.15.4");
			}
			const auto started = ::GetTickCount64();
			for (;;) {
				if (Cfg().blockSaves && saves::PollSteam() == saves::SteamPoll::kIncomplete) {
					// MGSV's own save path (Steam FileWrite) may be live: the quit watcher must ask it to
					// close and give it its grace, not kill it as if nothing could be saved.
					hidden::SetSavesBlocked(false);
					Log::Warn("saves: the quit watcher now asks MGSV to close (WM_CLOSE + grace) instead of killing it");
				}
				// The game normally calls DirectInput8Create within seconds; don't depend on it.
				if (::GetTickCount64() - started > 5000) {
					LateInit();
				}
				if (Cfg().capture) {
					capture::Poll();
				}
				::Sleep(1000);
			}
		}
	}

	bool Active() { return g_active.load(); }

	void OnProcessAttach(HMODULE a_self)
	{
		Log::Init(ModuleDir() + L"PhantomRingGuest.log", "guest");
		Log::Info("PhantomRing guest (dinput8.dll) {}", version::Describe());
		const auto id = CheckExe();
		if (!EnvActivated()) {
			Log::Info("PHANTOMRING is not 1: plain dinput8 proxy");
			return;
		}
		if (!id.matches) {
			Log::Info("{} is not mgsvtpp.exe 1.0.15.4 (stamp {:#x}, size {:#x}, base {:#x}): plain dinput8 proxy", Narrow(id.path), id.timeDateStamp,
				id.sizeOfImage, id.base);
			return;
		}
		MutableCfg().Load(ModuleDir() + L"PhantomRingGuest.ini");
		Log::SetDebug(Cfg().debugLog);
		Log::Info("PhantomRing guest activating in {}", Narrow(id.path));
		g_active.store(true);
		link::SetStatus(proto::Status::kBooting, "MGSV: starting");

		// Code hooks: created here (before the engine builds its Lua state and starts its frame loop),
		// each behind its own byte check.
		if (!Hooks::Init()) {
			Log::Error("MinHook unavailable -- nothing hooked");
			return;
		}
		vars::Init();
		link::SetWorkerTick(&vars::ApplyPendingLife);
		if (lua::Install()) {
			lua::SetPump(&pump::Run);
			state::SetLuaThreadWork(&LuaWork);
			state::SetLuaTickWork(&LuaTickWork);
			state::SetBuddyHooks({ .pose = [](logic::Vec3& p, float& yaw) { return skeleton::BuddyPose(p, yaw); },
				.hint = [](const logic::Vec3& at, float r, std::uint64_t ms) { skeleton::HintBuddy(at, r, ms); },
				.whistleHorse = &follow::WhistleHorseNow,
				.forget = &skeleton::ForgetBuddy });
			state::SetCommandResultSink(&ammo::OnResults);
		} else {
			link::SetStatus(proto::Status::kFailed, "MGSV: Lua hook unavailable (see PhantomRingGuest.log)");
		}
		camera::Install(&frame::OnPlayerCamera);
		skeleton::Install();
		if (Cfg().nativeShots) {
			combat::InstallShots();
		}
		if (Cfg().holdMode == HoldMode::kNative || Cfg().holdMode == HoldMode::kNativeTreadmill) {
			hold::Init();
		}
		follow::Install();
		blast::Install();
		throwing::Install();
		placed::Init();
		ammo::Init();
		sound::Install();
		menu::Install();
		if (Cfg().capture) {
			capture::InstallEarly();  // D3D11CreateDevice: the game creates its device right after DllMain
		}
		// Windows: MGSV's window thread starts right after DllMain; hide it from its first message on.
		hidden::Start(hidden::Load(ModuleDir() + L"PhantomRingGuest.ini"), &link::HostAlive);
		if (Cfg().blockSaves) {
			hidden::SetSavesBlocked(saves::InstallFileGuards());  // incomplete guards: the quit watcher treats saves as live
		} else {
			Log::Warn("saves: [Saves] Block=false -- MGSV WILL write its saves");
		}
		Hooks::EnableAll();
		if (Cfg().dumpDefaultLua) {
			DumpDefaultScript();
		}

		link::SetCommandHandler(&Command);
		link::Start(platform::ResolveMapping(Cfg().mapping, L"", platform::Detect().wine));  // empty/auto: the platform's default
		if (HANDLE t = ::CreateThread(nullptr, 0, &Maintenance, nullptr, 0, nullptr)) {
			::CloseHandle(t);
		}
		(void)a_self;
	}

	void LateInit()
	{
		if (!g_active.load()) {
			return;
		}
		std::call_once(g_lateOnce, [] {
			offline::Install();
			pad::Install();
			framepace::Install();
			if (Cfg().guardWindow && !hidden::Active()) {  // hidden mode installed it (guard included) in DllMain
				window::Install({});
			}
			Hooks::EnableAll();
			Log::Info("late init done");
		});
	}
}
