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
		mgsvBlasts = ini.GetBool("Combat", "MgsvBlasts", mgsvBlasts);

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

		loadoutPrimary = ini.Get("Loadout", "Primary", loadoutPrimary);
		loadoutPrimary2 = ini.Get("Loadout", "Primary2", loadoutPrimary2);
		loadoutSecondary = ini.Get("Loadout", "Secondary", loadoutSecondary);
		loadoutSupport = ini.Get("Loadout", "Support", loadoutSupport);
		boxPoster = ini.Get("Loadout", "BoxPoster", boxPoster);
		box = ini.Get("Loadout", "Box", box);

		capture = ini.GetBool("Capture", "Enabled", capture);
		hud = ini.GetBool("Capture", "Hud", hud);
		hudEvery = static_cast<int>(ini.GetInt("Capture", "HudEvery", hudEvery));

		luaOverride = Widen(ini.Get("Lua", "OverrideFile", Narrow(luaOverride)));
		dumpDefaultLua = ini.GetBool("Lua", "DumpDefault", dumpDefaultLua);
	}
}
