#include <windows.h>

#include "Config.h"

#include "Ini.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace phantom::guest
{
	namespace
	{
		Config g_config;

		std::string Lower(std::string a_s)
		{
			std::ranges::transform(a_s, a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::vector<std::uint16_t> Ids(const std::string& a_list)
		{
			std::vector<std::uint16_t> out;
			std::string                item;
			std::stringstream          ss(a_list);
			while (std::getline(ss, item, ',')) {
				try {
					if (!item.empty()) {
						out.push_back(static_cast<std::uint16_t>(std::stoul(item, nullptr, 0)));
					}
				} catch (...) {
					Log::Warn("ini: '{}' is not an equip id", item);
				}
			}
			return out;
		}

		// XInput button names ("rs", "lb+rb", "lb rb lt", "0x0080"); "lt" (a trigger, no button bit) sets a_lt. 0 = none known.
		std::uint16_t Buttons(const std::string& a_list, bool& a_lt)
		{
			static constexpr std::pair<const char*, std::uint16_t> kNames[] = { { "up", 0x0001 }, { "down", 0x0002 }, { "left", 0x0004 },
				{ "right", 0x0008 }, { "start", 0x0010 }, { "back", 0x0020 }, { "view", 0x0020 }, { "ls", 0x0040 }, { "l3", 0x0040 }, { "rs", 0x0080 },
				{ "r3", 0x0080 }, { "lb", 0x0100 }, { "rb", 0x0200 }, { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 } };
			std::uint16_t out = 0;
			std::string   item;
			std::string   list = Lower(a_list);
			std::ranges::replace(list, '+', ' ');
			std::ranges::replace(list, ',', ' ');
			std::stringstream ss(list);
			while (ss >> item) {
				if (item == "lt") {
					a_lt = true;
					continue;
				}
				bool known = false;
				for (const auto& [name, bit] : kNames) {
					if (item == name) {
						out = static_cast<std::uint16_t>(out | bit);
						known = true;
					}
				}
				if (!known) {
					try {
						out = static_cast<std::uint16_t>(out | std::stoul(item, nullptr, 0));
					} catch (...) {
						Log::Warn("ini: '{}' is not a pad button", item);
					}
				}
			}
			return out;
		}
	}

	const Config& Cfg() { return g_config; }
	Config&       MutableCfg() { return g_config; }

	void Config::Load(const std::wstring& a_path)
	{
		Ini ini;
		if (!ini.Load(a_path)) {
			Log::Info("no {} -- using defaults", Narrow(a_path));
		}

		debugLog = ini.GetBool("Guest", "Debug", debugLog);
		mapping = Widen(ini.Get("Link", "Mapping", ""));
		// The launcher may point both games at another mapping file (parallel test setups).
		if (wchar_t env[512]; ::GetEnvironmentVariableW(L"PHANTOMRING_MAPPING", env, 512) > 0) {
			mapping = env;
		}

		autoBoot = ini.GetBool("Boot", "Auto", autoBoot);
		mission = static_cast<int>(ini.GetInt("Boot", "Mission", mission));
		const auto pos = ini.Get("Boot", "Position", "");
		if (!pos.empty()) {
			std::stringstream ss(pos);
			char              comma = 0;
			ss >> position[0] >> comma >> position[1] >> comma >> position[2];
			hasPosition = !ss.fail();
			if (!hasPosition) {
				Log::Warn("ini: [Boot] Position '{}' is not 'x, y, z'", pos);
			}
		}
		rotYDeg = static_cast<float>(ini.GetFloat("Boot", "RotY", rotYDeg));
		disableSoldiers = ini.GetBool("Boot", "DisableSoldiers", disableSoldiers);
		clearMarkersSec = static_cast<int>(ini.GetInt("Boot", "ClearMarkersSec", clearMarkersSec));
		autoAim = ini.GetBool("Boot", "AutoAim", autoAim);
		invincible = ini.GetBool("Boot", "Invincible", invincible);
		clockTime = ini.Get("Boot", "Clock", clockTime);
		freezeClock = ini.GetBool("Boot", "FreezeClock", freezeClock);
		weather = ini.Get("Boot", "Weather", weather);
		pressButtons = ini.GetBool("Boot", "PressButtons", pressButtons);
		timeoutSec = static_cast<int>(ini.GetInt("Boot", "TimeoutSec", timeoutSec));

		buddy = Lower(ini.Get("Buddy", "Type", buddy));
		// Test runs pick the buddy without touching the installed ini (the player's): PHANTOMRING_BUDDY=horse.
		if (wchar_t env[32]; ::GetEnvironmentVariableW(L"PHANTOMRING_BUDDY", env, 32) > 0 && env[0]) {
			buddy = Lower(Narrow(env));
			Log::Info("ini: [Buddy] Type = {} (PHANTOMRING_BUDDY)", buddy);
		}
		buddyAhead = static_cast<float>(ini.GetFloat("Buddy", "Ahead", buddyAhead));
		buddySide = static_cast<float>(ini.GetFloat("Buddy", "Side", buddySide));
		buddyFollow = ini.GetBool("Buddy", "Follow", buddyFollow);
		buddyRevive = ini.GetBool("Buddy", "Revive", buddyRevive);
		buddyFollowDistance = static_cast<float>(ini.GetFloat("Buddy", "FollowDistance", buddyFollowDistance));
		buddyFollowEveryMs = static_cast<int>(ini.GetInt("Buddy", "FollowEveryMs", buddyFollowEveryMs));
		buddyFollowAhead = static_cast<float>(ini.GetFloat("Buddy", "FollowAhead", buddyFollowAhead));
		buddyFollowSide = static_cast<float>(ini.GetFloat("Buddy", "FollowSide", buddyFollowSide));
		buddyFollowLeadSec = static_cast<float>(ini.GetFloat("Buddy", "FollowLeadSec", buddyFollowLeadSec));
		buddyFollowSteer = ini.GetBool("Buddy", "FollowSteer", buddyFollowSteer);
		buddyFollowRespectCommands = ini.GetBool("Buddy", "FollowRespectCommands", buddyFollowRespectCommands);
		buddyFollowSideRunning = static_cast<float>(ini.GetFloat("Buddy", "FollowSideRunning", buddyFollowSideRunning));
		buddyFollowCatchUp = static_cast<float>(ini.GetFloat("Buddy", "FollowCatchUp", buddyFollowCatchUp));
		buddyFollowMethod = Lower(ini.Get("Buddy", "FollowMethod", buddyFollowMethod));
		buddyFollowModes = Lower(ini.Get("Buddy", "FollowModes", buddyFollowModes));
		buddyHorseFollowDistance = static_cast<float>(ini.GetFloat("Buddy", "HorseFollowDistance", buddyHorseFollowDistance));
		buddyHorseWhistleEveryMs = static_cast<int>(ini.GetInt("Buddy", "HorseWhistleEveryMs", buddyHorseWhistleEveryMs));
		buddyHorseTreadmill = ini.GetBool("Buddy", "HorseTreadmill", buddyHorseTreadmill);
		buddyHorseMoveRider = ini.GetBool("Buddy", "HorseMoveRider", buddyHorseMoveRider);

		const auto hold = Lower(ini.Get("Motion", "Hold", "native_treadmill"));
		holdMode = hold == "off"                ? HoldMode::kOff
		           : hold == "native"           ? HoldMode::kNative
		           : hold == "native_treadmill" ? HoldMode::kNativeTreadmill
		                                        : HoldMode::kTreadmill;
		radius = static_cast<float>(ini.GetFloat("Motion", "Radius", radius));
		teleportJump = static_cast<float>(ini.GetFloat("Motion", "TeleportJump", teleportJump));
		warpCooldownMs = static_cast<int>(ini.GetInt("Motion", "WarpCooldownMs", warpCooldownMs));
		treadmillOnVehicle = ini.GetBool("Motion", "TreadmillOnVehicle", treadmillOnVehicle);
		ridingCameraKeep = ini.GetBool("Motion", "RidingCameraKeep", ridingCameraKeep);
		treadmillMovesBuddy = ini.GetBool("Motion", "TreadmillMovesBuddy", treadmillMovesBuddy);
		velocitySmoothing = static_cast<float>(ini.GetFloat("Motion", "VelocitySmoothing", velocitySmoothing));
		warpLift = static_cast<float>(ini.GetFloat("Motion", "WarpLift", warpLift));
		diveFlag = ini.GetBool("Motion", "DiveFlag", diveFlag);
		diveConfirmMs = static_cast<int>(ini.GetInt("Motion", "DiveConfirmMs", diveConfirmMs));
		jump = ini.GetBool("Jump", "Enabled", jump);
		if (const auto button = ini.Get("Jump", "Button", ""); !button.empty()) {
			bool lt = false;
			const auto bits = Buttons(button, lt);
			if (bits && !lt) {
				jumpButton = bits;
			} else {
				Log::Warn("ini: [Jump] Button '{}' is no pad button (ls rs a b x y lb rb up down left right back, or 0x hex) -- keeping {:#06x}", button,
					jumpButton);
			}
		}
		{
			bool lt = false;
			jumpPassWith = Buttons(ini.Get("Jump", "PassWith", "lb rb lt up down left right"), lt);
			jumpPassAiming = lt;
		}
		jumpLift = std::clamp(static_cast<float>(ini.GetFloat("Jump", "Lift", jumpLift)), 0.2f, 1.5f);
		jumpMaxAirMs = std::clamp(static_cast<int>(ini.GetInt("Jump", "MaxAirMs", jumpMaxAirMs)), 500, 5000);
		if (jump) {
			Log::Info("ini: [Jump] button {:#06x} (MGSV's own with {:#06x}{} held), lift {:.2f} m", jumpButton, jumpPassWith, jumpPassAiming ? " or LT" : "", jumpLift);
		}
		jobSleepUs = std::clamp(static_cast<int>(ini.GetInt("Pacing", "JobSleepUs", jobSleepUs)), 0, 1000);
		waitProbe = ini.GetBool("Pacing", "WaitProbe", waitProbe);

		offline = ini.GetBool("Online", "Offline", offline);
		padDuringBoot = ini.GetBool("Input", "PadDuringBoot", padDuringBoot);
		hideDirectInputControllers = ini.GetBool("Input", "HideDirectInputControllers", hideDirectInputControllers);
		rumble = ini.GetBool("Input", "Rumble", rumble);
		guardWindow = ini.GetBool("Input", "GuardWindow", guardWindow);

		filmHeightMm = static_cast<float>(ini.GetFloat("Camera", "FilmHeightMm", filmHeightMm));
		fovOverrideDeg = static_cast<float>(ini.GetFloat("Camera", "FovOverrideDeg", fovOverrideDeg));
		nearPlane = static_cast<float>(ini.GetFloat("Camera", "Near", nearPlane));

		lifeScale = static_cast<float>(ini.GetFloat("Combat", "LifeScale", lifeScale));
		nativeLifeWrite = ini.GetBool("Combat", "NativeLifeWrite", nativeLifeWrite);
		hurtFeedback = ini.GetBool("Combat", "HurtFeedback", hurtFeedback);
		regenGate = ini.GetBool("Combat", "RegenGate", regenGate);
		regenDelaySec = static_cast<float>(ini.GetFloat("Combat", "RegenDelaySec", regenDelaySec));
		regenRampSec = static_cast<float>(ini.GetFloat("Combat", "RegenRampSec", regenRampSec));
		regenRate = static_cast<float>(ini.GetFloat("Combat", "RegenRate", regenRate));
		hurtBlood = ini.GetBool("Combat", "HurtBlood", hurtBlood);
		deathCollapse = ini.GetBool("Combat", "DeathCollapse", deathCollapse);
		fireFromAmmo = ini.GetBool("Combat", "FireFromAmmo", fireFromAmmo);
		nativeShots = ini.GetBool("Combat", "NativeShots", nativeShots);
		melee = ini.GetBool("Combat", "Melee", melee);
		meleeShapeGeometry = ini.GetBool("Combat", "MeleeShapeGeometry", meleeShapeGeometry);
		muzzleBone = static_cast<int>(ini.GetInt("Combat", "MuzzleBone", muzzleBone));
		tranqWeapons = Ids(ini.Get("Combat", "TranqWeapons", ""));
		stunWeapons = Ids(ini.Get("Combat", "StunWeapons", ""));
		blasts = ini.GetBool("Combat", "Blasts", blasts);
		throws = ini.GetBool("Combat", "Throws", throws);
		placed = ini.GetBool("Combat", "Placed", placed);
		decoys = ini.GetBool("Combat", "Decoys", decoys);
		decoyPopInMgsv = ini.GetBool("Combat", "DecoyPopInMgsv", decoyPopInMgsv);
		mgsvBlasts = ini.GetBool("Combat", "MgsvBlasts", mgsvBlasts);
		c4Detonator = ini.GetBool("Combat", "C4Detonator", c4Detonator);
		c4Backup = ini.GetBool("Combat", "C4Backup", c4Backup);
		c4Buttons = static_cast<std::uint32_t>(ini.GetInt("Combat", "C4Buttons", c4Buttons));
		c4GraceMs = std::clamp<std::int64_t>(ini.GetInt("Combat", "C4GraceMs", c4GraceMs), 0, 5000);
		c4DetonateAll = ini.GetBool("Combat", "C4DetonateAll", c4DetonateAll);

		blockSaves = ini.GetBool("Saves", "Block", blockSaves);
		saveSinkDir = Widen(ini.Get("Saves", "SinkDir", Narrow(saveSinkDir)));

		// on/off (true/false); the older keep / mute_bgm / mute_all still parse.
		const auto music = Lower(ini.Get("Audio", "Music", "off"));
		this->music = music == "keep" || music == "on" || music == "true" ? MusicMode::kKeep
		              : music == "mute_all"                              ? MusicMode::kMuteAll
		                                                                 : MusicMode::kMuteBgm;
		ambience = ini.GetBool("Audio", "Ambience", ambience) && this->music != MusicMode::kMuteAll;
		radio = ini.GetBool("Audio", "Radio", radio) && this->music != MusicMode::kMuteAll;
		walkerVolume = static_cast<float>(ini.GetFloat("Audio", "WalkerVolume", walkerVolume));

		safePauseMenu = ini.GetBool("Menu", "SafePauseMenu", safePauseMenu);
		blockMissions = ini.GetBool("Menu", "BlockMissions", blockMissions);
		buddyDismiss = ini.GetBool("Menu", "BuddyDismiss", buddyDismiss);
		idroidRetryMs = std::clamp(static_cast<int>(ini.GetInt("Menu", "IDroidRetryMs", idroidRetryMs)), 0, 10000);

		loadoutPrimary = ini.Get("Loadout", "Primary", loadoutPrimary);
		loadoutPrimary2 = ini.Get("Loadout", "Primary2", loadoutPrimary2);
		loadoutSecondary = ini.Get("Loadout", "Secondary", loadoutSecondary);
		loadoutSupport = ini.Get("Loadout", "Support", loadoutSupport);
		boxPoster = ini.Get("Loadout", "BoxPoster", boxPoster);
		box = ini.Get("Loadout", "Box", box);

		capture = ini.GetBool("Capture", "Enabled", capture);
		hud = ini.GetBool("Capture", "Hud", hud);
		captureRevalidate = ini.GetBool("Capture", "Revalidate", captureRevalidate);
		hudEvery = static_cast<int>(ini.GetInt("Capture", "HudEvery", hudEvery));
		projectileMeshes = ini.GetBool("Capture", "ProjectileMeshes", projectileMeshes);

		cigarTimeSkip = ini.GetBool("Cigar", "TimeSkip", cigarTimeSkip);
		cigarClockScale = std::clamp(static_cast<float>(ini.GetFloat("Cigar", "ClockScale", cigarClockScale)), 0.0f, 10000.0f);

		fultonAnchor = ini.GetBool("Fulton", "Enabled", fultonAnchor);
		fultonMethod = ini.Get("Fulton", "Method", fultonMethod);
		if (fultonMethod != "proxy" && fultonMethod != "anchor") {
			fultonMethod = "proxy";
		}
		fultonReach = std::clamp(static_cast<float>(ini.GetFloat("Fulton", "Reach", fultonReach)), 0.5f, 10.0f);
		fultonLeave = std::clamp(static_cast<float>(ini.GetFloat("Fulton", "Leave", fultonLeave)), fultonReach + 0.5f, 20.0f);
		fultonCandidates = std::clamp(static_cast<float>(ini.GetFloat("Fulton", "CandidateRadius", fultonCandidates)), 10.0f, 300.0f);
		fultonRise = std::clamp(static_cast<float>(ini.GetFloat("Fulton", "Rise", fultonRise)), 0.5f, 20.0f);
		cqc = ini.GetBool("Cqc", "Enabled", cqc);
		cqcMgsvHold = ini.GetBool("Cqc", "MgsvHold", cqcMgsvHold);
		{
			const auto u = [&](const char* a_key, std::uint32_t& a_value, std::int64_t a_max) {
				a_value = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ini.GetInt("Cqc", a_key, a_value), 0, a_max));
			};
			u("HoldMs", cqcHoldMs, 5000);
			u("ChokeMs", cqcChokeMs, 60000);
			u("MaxHoldMs", cqcMaxHoldMs, 120000);
			u("CooldownMs", cqcCooldownMs, 10000);
			u("Buttons", cqcButtons, 0xFFFFFFFF);
			u("AimButtons", cqcAimButtons, 0xFFFFFFFF);
			u("InterrogateButtons", cqcInterrogateButtons, 0xFFFFFFFF);
			u("ChokeButtons", cqcChokeButtons, 0xFFFFFFFF);
		}
		cqcThrowStick = std::clamp(static_cast<float>(ini.GetFloat("Cqc", "ThrowStick", cqcThrowStick)), 0.05f, 1.01f);
		cqcRange = std::clamp(static_cast<float>(ini.GetFloat("Cqc", "Range", cqcRange)), 0.3f, 5.0f);
		proxies = ini.GetBool("Proxies", "Enabled", proxies);
		proxyMax = std::clamp(static_cast<int>(ini.GetInt("Proxies", "Max", proxyMax)), 0, 32);
		proxyMaxDistance = static_cast<float>(ini.GetFloat("Proxies", "MaxDistance", proxyMaxDistance));
		proxySnakeClearance = static_cast<float>(ini.GetFloat("Proxies", "SnakeClearance", proxySnakeClearance));
		proxyBuddyClearance = static_cast<float>(ini.GetFloat("Proxies", "BuddyClearance", proxyBuddyClearance));
		proxyParkDepth = static_cast<float>(ini.GetFloat("Proxies", "ParkDepth", proxyParkDepth));
		proxyTableLagMs = std::clamp(static_cast<int>(ini.GetInt("Proxies", "TableLagMs", proxyTableLagMs)), 0, 500);
		proxyAutoAim = ini.GetBool("Proxies", "AutoAim", proxyAutoAim);
		proxyKeepMarkers = ini.GetBool("Proxies", "KeepMarkers", proxyKeepMarkers);
		proxyCaptureRadius = static_cast<float>(ini.GetFloat("Proxies", "CaptureRadius", proxyCaptureRadius));
		proxyInert = ini.GetBool("Proxies", "Inert", proxyInert);
		proxyNoAssignRoute = ini.GetBool("Proxies", "NoAssignRoute", proxyNoAssignRoute);
		proxyPhaseLock = ini.GetBool("Proxies", "PhaseLock", proxyPhaseLock);
		proxyAlertGiveUpSec = std::clamp(static_cast<float>(ini.GetFloat("Proxies", "AlertGiveUpSec", proxyAlertGiveUpSec)), 2.0f, 600.0f);
		proxyHeight = ini.Get("Proxies", "Height", proxyHeight);
		if (proxyHeight != "ground" && proxyHeight != "enemy") {
			proxyHeight = "ground";
		}
		proxyHeightTolerance = std::clamp(static_cast<float>(ini.GetFloat("Proxies", "HeightTolerance", proxyHeightTolerance)), 0.5f, 20.0f);
		proxyAimHeightFix = ini.GetBool("Proxies", "AimHeightFix", proxyAimHeightFix);

		luaOverride = Widen(ini.Get("Lua", "OverrideFile", Narrow(luaOverride)));
		dumpDefaultLua = ini.GetBool("Lua", "DumpDefault", dumpDefaultLua);
	}
}
