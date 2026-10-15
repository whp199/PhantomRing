#include <windows.h>

#include "Config.h"

#include "ExplosionLogic.h"
#include "Ini.h"
#include "Log.h"
#include "Platform.h"
#include "phantomring_protocol.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace phantom::er
{
	namespace
	{
		float Sign(double a_value) { return a_value < 0 ? -1.0f : 1.0f; }

		// "x, y, z" -> a_out[0..n) (keeps the defaults for a missing or short list)
		void Floats(const std::string& a_text, float* a_out, int a_n)
		{
			float       v[4]{};
			int         got = 0;
			const char* p = a_text.c_str();
			while (got < a_n && *p) {
				char*        end = nullptr;
				const double x = std::strtod(p, &end);
				if (end == p) {
					break;
				}
				v[got++] = static_cast<float>(x);
				p = end;
				while (*p == ',' || *p == ' ' || *p == '\t') {
					++p;
				}
			}
			if (got == a_n) {
				std::copy(v, v + a_n, a_out);
			}
		}

		// "a, b, c" (decimal or 0x hex) -> a_out, replacing the defaults; empty/missing keeps them, "none" empties the list.
		void IntList(const Ini& a_ini, const char* a_section, const char* a_key, std::vector<int>& a_out, long a_max)
		{
			const auto text = a_ini.Get(a_section, a_key);
			if (text.empty()) {
				return;
			}
			a_out.clear();
			std::stringstream list(text);
			std::string       item;
			while (std::getline(list, item, ',')) {
				char*      end = nullptr;
				const long v = std::strtol(item.c_str(), &end, 0);
				if (end != item.c_str() && v >= 0 && v <= a_max) {
					a_out.push_back(static_cast<int>(v));
				}
			}
		}

		int Auto(const std::string& a_text)
		{
			if (a_text.empty() || a_text == "auto" || a_text == "0") {
				return 0;
			}
			return a_text.starts_with('-') ? -1 : 1;
		}
	}

	Config& Cfg()
	{
		static Config config;
		return config;
	}

	bool Config::Load(const std::wstring& a_path)
	{
		Ini  ini;
		const bool found = ini.Load(a_path);

		// Empty/auto = the platform's default (/dev/shm file under Wine, Local\PhantomRing_v1 on Windows); the
		// launcher may point both games at another mapping (parallel test setups): common/Platform.h.
		{
			wchar_t    env[512];
			const auto n = ::GetEnvironmentVariableW(L"PHANTOMRING_MAPPING", env, 512);
			mapping = platform::ResolveMapping(Widen(ini.Get("Link", "mapping")), n > 0 && n < 512 ? std::wstring_view(env, n) : std::wstring_view(), platform::Detect().wine);
		}

		debug = ini.GetBool("Debug", "debug", debug);
		verifySha256 = ini.GetBool("Debug", "verify_sha256", verifySha256);

		input = ini.GetBool("Features", "input", input);
		puppet = ini.GetBool("Features", "puppet", puppet);
		camera = ini.GetBool("Features", "camera", camera);
		console = ini.GetBool("Features", "console", console);
		actors = ini.GetBool("Features", "actors", actors);
		debugDraw = ini.GetBool("Features", "debug_draw", debugDraw);
		buddy = ini.GetBool("Features", "buddy", buddy);

		userIndex = static_cast<int>(ini.GetInt("Input", "user_index", userIndex));
		allowButtons = static_cast<std::uint16_t>(ini.GetInt("Input", "allow_buttons", allowButtons));
		menuLearn = ini.GetBool("Input", "menu_learn", menuLearn);
		menuLatchMs = static_cast<int>(ini.GetInt("Input", "menu_latch_ms", menuLatchMs));
		promptButton = static_cast<std::uint16_t>(ini.GetInt("Input", "prompt_button", promptButton));
		interactionHudState = static_cast<int>(ini.GetInt("Input", "interaction_hud_state", interactionHudState));
		interactionMinMs = static_cast<int>(ini.GetInt("Input", "interaction_min_ms", interactionMinMs));
		interactionMaxMs = static_cast<int>(ini.GetInt("Input", "interaction_max_ms", interactionMaxMs));
		viewHoldMs = static_cast<int>(ini.GetInt("Input", "view_hold_ms", viewHoldMs));
		guestIdroidStartMs = static_cast<int>(ini.GetInt("Input", "guest_idroid_start_ms", guestIdroidStartMs));
		guestMenuOwnsPad = ini.GetBool("Input", "guest_menu_owns_pad", guestMenuOwnsPad);
		padGlyphDevice = static_cast<int>(ini.GetInt("Input", "pad_glyph_device", padGlyphDevice));
		IntList(ini, "Input", "release_prompts", releasePrompts, 0x7FFFFFFF);
		releaseMs = static_cast<int>(ini.GetInt("Input", "release_ms", releaseMs));
		startWaitMs = static_cast<int>(ini.GetInt("Input", "start_wait_ms", startWaitMs));
		chordMs = static_cast<int>(ini.GetInt("Input", "chord_ms", chordMs));
		chordWindowMs = static_cast<int>(ini.GetInt("Input", "chord_window_ms", chordWindowMs));
		IntList(ini, "Input", "rest_prompts", restPrompts, 0x7FFFFFFF);
		IntList(ini, "Input", "menu_keys", menuKeys, 0xFF);
		IntList(ini, "Input", "ladder_prompts", ladderPrompts, 0x7FFFFFFF);
		ladderPressDelayMs = static_cast<int>(ini.GetInt("Input", "ladder_press_delay_ms", ladderPressDelayMs));
		ladderWaitMs = static_cast<int>(ini.GetInt("Input", "ladder_wait_ms", ladderWaitMs));
		ladderSettleMs = static_cast<int>(ini.GetInt("Input", "ladder_settle_ms", ladderSettleMs));
		IntList(ini, "Input", "menu_ui_states", menuUiStates, 0x45);

		hideMode = ini.Get("Puppet", "hide_mode", hideMode);
		motion = ini.Get("Puppet", "motion", motion);
		if (motion != "kinematic" && motion != "root_motion") {
			Log::Warn("[Puppet] motion = {} is not kinematic|root_motion; using kinematic", motion);
			motion = "kinematic";
		}
		catchUp = static_cast<float>(ini.GetFloat("Puppet", "catch_up", catchUp));
		unstick = ini.GetBool("Puppet", "unstick", unstick);
		unstickSeconds = static_cast<float>(ini.GetFloat("Puppet", "unstick_seconds", unstickSeconds));
		stallSeconds = static_cast<float>(ini.GetFloat("Puppet", "stall_seconds", stallSeconds));
		fultonRunes = ini.GetBool("Fulton", "runes", fultonRunes);
		ceiling = ini.GetBool("Puppet", "ceiling", ceiling);
		heightStand = static_cast<float>(ini.GetFloat("Puppet", "height_stand", heightStand));
		heightCrouch = static_cast<float>(ini.GetFloat("Puppet", "height_crouch", heightCrouch));
		heightProne = static_cast<float>(ini.GetFloat("Puppet", "height_prone", heightProne));
		heightRide = static_cast<float>(ini.GetFloat("Puppet", "height_ride", heightRide));
		fallWatchSeconds = static_cast<float>(ini.GetFloat("Puppet", "fall_watch_seconds", fallWatchSeconds));
		fallWatchDrop = static_cast<float>(ini.GetFloat("Puppet", "fall_watch_drop", fallWatchDrop));
		hitHook = ini.GetBool("Puppet", "hit_hook", hitHook);
		noDead = ini.GetBool("Puppet", "nodead", noDead);
		noDamage = ini.GetBool("Puppet", "nodamage", noDamage);
		mirrorLife = ini.GetBool("Puppet", "mirror_life", mirrorLife);
		forwardHeals = ini.GetBool("Puppet", "forward_heals", forwardHeals);
		killOnGuestDeath = ini.GetBool("Puppet", "kill_on_guest_death", killOnGuestDeath);
		maxSpeed = static_cast<float>(ini.GetFloat("Puppet", "max_speed", maxSpeed));
		maxSpeedVehicle = static_cast<float>(ini.GetFloat("Puppet", "max_speed_vehicle", maxSpeedVehicle));
		maxSpeedHorse = static_cast<float>(ini.GetFloat("Puppet", "max_speed_horse", maxSpeedHorse));
		teleportDrop = static_cast<float>(ini.GetFloat("Puppet", "teleport_drop", teleportDrop));
		resyncDistance = static_cast<float>(ini.GetFloat("Puppet", "resync_distance", resyncDistance));
		radius = static_cast<float>(ini.GetFloat("Puppet", "radius", radius));
		stepUp = static_cast<float>(ini.GetFloat("Puppet", "step_up", stepUp));
		stepUpVehicle = static_cast<float>(ini.GetFloat("Puppet", "step_up_vehicle", stepUpVehicle));
		stepUpHorse = static_cast<float>(ini.GetFloat("Puppet", "step_up_horse", stepUpHorse));
		radiusVehicle = static_cast<float>(ini.GetFloat("Puppet", "radius_vehicle", radiusVehicle));
		radiusHorse = static_cast<float>(ini.GetFloat("Puppet", "radius_horse", radiusHorse));
		snapDown = static_cast<float>(ini.GetFloat("Puppet", "snap_down", snapDown));
		chestHeight = static_cast<float>(ini.GetFloat("Puppet", "chest_height", chestHeight));
		gravity = static_cast<float>(ini.GetFloat("Puppet", "gravity", gravity));
		rayFilter = static_cast<std::uint32_t>(ini.GetInt("Puppet", "ray_filter", rayFilter));
		groundFilter = static_cast<std::uint32_t>(ini.GetInt("Puppet", "ground_filter", groundFilter));
		wallFilter = static_cast<std::uint32_t>(ini.GetInt("Puppet", "wall_filter", wallFilter));
		wallFilterAlso = static_cast<std::uint32_t>(ini.GetInt("Puppet", "wall_filter_also", wallFilterAlso));
		groundKeep = static_cast<std::uint32_t>(ini.GetInt("Puppet", "ground_keep", groundKeep));
		groundLayers = static_cast<std::uint32_t>(ini.GetInt("Puppet", "ground_layers", groundLayers)) & 0xFF;
		groundProbes = ini.GetBool("Puppet", "ground_probes", groundProbes);
		// dive = hop (roadmap 2.9)
		hop = ini.GetBool("Puppet", "hop", hop);
		hopHeight = static_cast<float>(ini.GetFloat("Puppet", "hop_height", hopHeight));
		hopLength = static_cast<float>(ini.GetFloat("Puppet", "hop_length", hopLength));
		hopStep = static_cast<float>(ini.GetFloat("Puppet", "hop_step", hopStep));
		hopMinRise = static_cast<float>(ini.GetFloat("Puppet", "hop_min_rise", hopMinRise));
		hopWaitMs = static_cast<int>(ini.GetInt("Puppet", "hop_wait_ms", hopWaitMs));
		jump = ini.GetBool("Puppet", "jump", jump);
		jumpApex = std::clamp(static_cast<float>(ini.GetFloat("Puppet", "jump_apex", jumpApex)), 0.2f, 3.0f);
		jumpMinRoom = std::clamp(static_cast<float>(ini.GetFloat("Puppet", "jump_min_room", jumpMinRoom)), 0.05f, 1.0f);
		jumpAirControl = std::clamp(static_cast<float>(ini.GetFloat("Puppet", "jump_air_control", jumpAirControl)), 0.0f, 50.0f);
		jumpAirStep = std::clamp(static_cast<float>(ini.GetFloat("Puppet", "jump_air_step", jumpAirStep)), 0.05f, 0.6f);
		jumpCameraHold = ini.GetBool("Puppet", "jump_camera_hold", jumpCameraHold);

		mirrorX = Sign(ini.GetFloat("Axes", "mirror_x", mirrorX));
		mgsvYawSign = Sign(ini.GetFloat("Axes", "mgsv_yaw_sign", mgsvYawSign));
		erForward = Auto(ini.Get("Axes", "er_forward", "auto"));
		mgsvCamForward = Sign(ini.GetFloat("Axes", "mgsv_cam_forward", mgsvCamForward));
		mgsvRootOffsetY = static_cast<float>(ini.GetFloat("Axes", "mgsv_root_offset_y", mgsvRootOffsetY));

		cameraTaskGroup = static_cast<int>(ini.GetInt("Camera", "task_group", cameraTaskGroup));
		camForwardRow = Auto(ini.Get("Camera", "forward_row", "auto"));
		fovScale = static_cast<float>(ini.GetFloat("Camera", "fov_scale", fovScale));
		fovOffset = static_cast<float>(ini.GetFloat("Camera", "fov_offset", fovOffset));
		writeNear = ini.GetBool("Camera", "write_near", writeNear);
		cameraCollision = ini.GetBool("Camera", "collision", cameraCollision);
		syncChrCam = ini.GetBool("Camera", "sync_chr_cam", syncChrCam);
		syncCompassCam = ini.GetBool("Camera", "sync_compass_cam", syncCompassCam);
		eyeHeight = static_cast<float>(ini.GetFloat("Camera", "eye_height", eyeHeight));
		eyeHeightVehicle = static_cast<float>(ini.GetFloat("Camera", "eye_height_vehicle", eyeHeightVehicle));
		eyeHeightHorse = static_cast<float>(ini.GetFloat("Camera", "eye_height_horse", eyeHeightHorse));
		collisionMargin = static_cast<float>(ini.GetFloat("Camera", "collision_margin", collisionMargin));
		collisionRadius = static_cast<float>(ini.GetFloat("Camera", "collision_radius", collisionRadius));
		collisionEaseOut = static_cast<float>(ini.GetFloat("Camera", "collision_ease_out", collisionEaseOut));
		verticalSmoothing = static_cast<float>(ini.GetFloat("Camera", "vertical_smoothing", verticalSmoothing));
		smooth = ini.GetBool("Camera", "smooth", smooth);
		if (const auto d = ini.Get("Camera", "smooth_delay", "auto"); d != "auto" && !d.empty()) {
			smoothDelay = static_cast<float>(std::strtod(d.c_str(), nullptr));
		}
		smoothExtrapolate = static_cast<float>(ini.GetFloat("Camera", "smooth_extrapolate", smoothExtrapolate));
		smoothMotion = static_cast<int>(ini.GetInt("Camera", "smooth_motion", smoothMotion));

		actorRadius = static_cast<float>(ini.GetFloat("Actors", "radius", actorRadius));
		actorMax = static_cast<int>(ini.GetInt("Actors", "max", actorMax));
		if (actorMax < 0 || actorMax > static_cast<int>(proto::kMaxActors)) {
			actorMax = static_cast<int>(proto::kMaxActors);
		}

		drawSkeleton = ini.GetBool("Draw", "skeleton", drawSkeleton);
		drawBuddy = ini.GetBool("Draw", "buddy", drawBuddy);
		drawTracers = ini.GetBool("Draw", "tracers", drawTracers);
		tracerSeconds = static_cast<float>(ini.GetFloat("Draw", "tracer_seconds", tracerSeconds));

		auto f = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Combat", a_key, a_value)); };
		auto i = [&](const char* a_key, int& a_value) { a_value = static_cast<int>(ini.GetInt("Combat", a_key, a_value)); };
		f("trace_range", traceRange);
		damage = ini.GetBool("Combat", "damage", damage);
		route = ini.Get("Combat", "route", route);
		if (route != "auto" && route != "bullet" && route != "hp") {
			Log::Warn("[Combat] route = {} is not auto|bullet|hp; using auto", route);
			route = "auto";
		}
		targets = ini.Get("Combat", "targets", targets);
		if (targets != "hostile" && targets != "all") {
			Log::Warn("[Combat] targets = {} is not hostile|all; using hostile", targets);
			targets = "hostile";
		}
		bulletId = static_cast<std::uint32_t>(ini.GetInt("Combat", "bullet_id", bulletId));
		bulletHideSfx = ini.GetBool("Combat", "bullet_hide_sfx", bulletHideSfx);
		f("bullet_offset", bulletOffset);
		f("bullet_damage_mult", bulletDamageMult);
		i("verify_ms", verifyMs);
		i("verify_failures", verifyFailures);
		f("units_per_life", unitsPerLife);
		f("big_from", bigFrom);
		f("boss_from", bossFrom);
		f("boss_lives", bossLives);
		f("share_mult", shareMult);
		f("head_fraction", headFraction);
		f("headshot_mult", headshotMult);
		f("headshot_mult_boss", headshotMultBoss);
		f("headshot_min_lives", headshotMinLives);
		f("buddy_mult", buddyMult);
		i("max_damage", maxDamage);
		i("tranq_sleep", tranqSleep);
		i("tranq_sleep_head", tranqSleepHead);
		i("choke_sleep", chokeSleep);
		i("gas_sleep", gasSleep);
		f("tranq_share", tranqShare);
		f("tranq_share_head", tranqShareHead);
		f("choke_share", chokeShare);
		f("gas_share", gasShare);
		i("sleep_correction", sleepCorrection);
		f("stun_poise_mult", stunPoiseMult);
		i("sleep_speffect_base", sleepSpEffectBase);
		i("sleep_speffect_step", sleepSpEffectStep);
		i("sleep_speffect_count", sleepSpEffectCount);
		f("melee_punch", meleePunch);
		f("melee_kick", meleeKick);
		f("melee_throw", meleeThrow);
		f("melee_throw_poise", meleeThrowPoise);
		f("melee_strike_poise", meleeStrikePoise);
		f("melee_range", meleeRange);
		f("melee_arc_deg", meleeArcDeg);
		f("explosion_radius", explosionRadius);
		f("max_hits_per_second", maxHitsPerSecond);
		f("capsule_radius", capsuleRadius);
		f("capsule_height", capsuleHeight);
		fallDamage = ini.Get("Combat", "fall_damage", fallDamage);
		if (fallDamage != "auto" && fallDamage != "host" && fallDamage != "off") {
			Log::Warn("[Combat] fall_damage = {} is not auto|host|off; using auto", fallDamage);
			fallDamage = "auto";
		}
		f("fall_hurt_speed", fallHurtSpeed);
		f("fall_lethal_speed", fallLethalSpeed);
		f("guest_life_scale", guestLifeScale);
		topUp = ini.GetBool("Combat", "top_up", topUp);
		damageRoute = ini.Get("Combat", "damage_route", damageRoute);
		if (damageRoute != "topup" && damageRoute != "speffect") {
			Log::Warn("[Combat] damage_route = {} is not topup|speffect; using topup", damageRoute);
			damageRoute = "topup";
		}
		i("damage_speffect_base", damageSpEffectBase);
		i("damage_speffect_digits", damageSpEffectDigits);
		damageSpEffectDigits = std::clamp(damageSpEffectDigits, 1, 5);
		f("speffect_bullet_share", speffectBulletShare);
		i("speffect_first_correction", speffectFirstCorrection);
		speffectFirstCorrection = std::clamp(speffectFirstCorrection, 1, 100);
		i("speffect_guard_ms", speffectGuardMs);
		i("speffect_template", speffectTemplate);
		i("speffect_wait_ms", speffectWaitMs);
		hurtShare = ini.GetBool("Combat", "hurt_share", hurtShare);
		f("hurt_mult", hurtMult);
		explosionFx = ini.Get("Combat", "explosion_fx", explosionFx);
		if (!explosion::ParseMode(explosionFx)) {
			Log::Warn("[Combat] explosion_fx = {} is not auto|bullet|sfx|off; using auto", explosionFx);
			explosionFx = "auto";
		}
		for (std::size_t k = 0; k < explosion::kKinds; ++k) {
			const auto key = "explosion_" + std::string(explosion::Name(static_cast<explosion::Kind>(k)));
			const auto text = ini.Get("Combat", key.c_str(), "");
			if (text.empty()) {
				continue;
			}
			if (const auto look = explosion::ParseLook(text)) {
				explosionTemplate[k] = look->templateId;
				explosionSfx[k] = look->sfx;
			} else {
				Log::Warn("[Combat] {} = {} is not '<bullet id> [sfx id]'; keeping {} {}", key, text, explosionTemplate[k], explosionSfx[k]);
			}
		}
		if (const auto text = ini.Get("Combat", "explosion_carriers", ""); !text.empty()) {
			if (const auto ids = explosion::ParseIds(text)) {
				std::copy(ids->begin(), ids->end(), explosionCarrier.begin());
			} else {
				Log::Warn("[Combat] explosion_carriers = {} is not {} different bullet ids; keeping the defaults", text, explosion::kKinds);
			}
		}
		i("explosion_max_live", explosionMaxLive);
		f("explosion_cloud_life", explosionCloudLife);
		breakObjects = ini.GetBool("Combat", "break_objects", breakObjects);
		breakerAtk = static_cast<std::uint32_t>(ini.GetInt("Combat", "breaker_atk", breakerAtk));
		i("breaker_obj_attack", breakerObjAttack);
		explosionAtk = static_cast<std::uint32_t>(ini.GetInt("Combat", "explosion_atk", explosionAtk));
		i("explosion_obj_attack", explosionObjAttack);
		f("breaker_radius", breakerRadius);
		f("breaker_speed", breakerSpeed);
		f("breaker_lead", breakerLead);
		i("shrapnel_count", shrapnelCount);
		breakerHitsCharacters = ini.GetBool("Combat", "breaker_hits_characters", breakerHitsCharacters);
		explosionBreaks = ini.GetBool("Combat", "explosion_breaks", explosionBreaks);
		f("breaker_per_second", breakerPerSecond);
		roundFlight = ini.GetBool("Combat", "round_flight", roundFlight);
		f("round_speed_rocket", roundSpeedRocket);
		f("round_speed_rocket_boost", roundSpeedRocketBoost);
		f("round_rocket_boost_after", roundRocketBoostAfter);
		f("round_speed_grenade", roundSpeedGrenade);
		flameFx = ini.GetBool("Combat", "flame_fx", flameFx);
		f("flame_per_second", flamePerSecond);
		f("flame_speed", flameSpeed);
		muzzleFlash = ini.GetBool("Combat", "muzzle_flash", muzzleFlash);
		f("muzzle_per_second", muzzlePerSecond);
		f("muzzle_life", muzzleLife);
		f("muzzle_ahead", muzzleAhead);
		muzzleTranq = ini.GetBool("Combat", "muzzle_tranq", muzzleTranq);

		{
			auto cf = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Cqc", a_key, a_value)); };
			cqc = ini.GetBool("Cqc", "enabled", cqc);
			cf("range", cqcRange);
			cf("arc_deg", cqcArcDeg);
			cf("boss_bigness", cqcBossBigness);
			cf("hold_seconds", cqcHoldSeconds);
			cf("leave_distance", cqcLeaveDistance);
			cf("grab_poise", cqcGrabPoise);
			cf("throw_poise", cqcThrowPoise);
			cqcThrowDmgLevel = static_cast<int>(ini.GetInt("Cqc", "throw_dmg_level", cqcThrowDmgLevel));
			cf("stagger_poise", cqcStaggerPoise);
			cqcInterrogate = ini.GetBool("Cqc", "interrogate", cqcInterrogate);
			cqcBossHp = static_cast<int>(ini.GetInt("Cqc", "boss_hp", cqcBossHp));
			cf("hold_cooldown", cqcHoldCooldown);
			cqcHitCorrection = static_cast<int>(ini.GetInt("Cqc", "hit_correction", cqcHitCorrection));
			cqcSettleMs = static_cast<int>(ini.GetInt("Cqc", "settle_ms", cqcSettleMs));
			cqcRetryMs = static_cast<int>(ini.GetInt("Cqc", "retry_ms", cqcRetryMs));
		}
		auto sf = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Stealth", a_key, a_value)); };
		boxHide = ini.GetBool("Stealth", "box_hide", boxHide);
		stealthSpEffect = static_cast<std::int32_t>(ini.GetInt("Stealth", "speffect", stealthSpEffect));
		sf("still_speed", boxStillSpeed);
		sf("slow_speed", boxSlowSpeed);
		sf("slow_grace", boxSlowGrace);
		sf("reveal_distance", boxRevealDistance);
		boxPoster = ini.GetBool("Stealth", "poster", boxPoster);
		sf("poster_range", boxPosterRange);
		sf("poster_stare", boxPosterStare);
		sf("poster_cooldown", boxPosterCooldown);
		i("explosion_max_bursts", explosionMaxBursts);
		f("explosion_per_second", explosionPerSecond);
		f("explosion_cloud_repeat", explosionCloudRepeat);
		explosionGround = ini.GetBool("Combat", "explosion_ground", explosionGround);

		progression = ini.Get("Progression", "damage_scaling", progression);
		if (progression != "off" && progression != "level" && progression != "weapon" && progression != "both") {
			Log::Warn("[Progression] damage_scaling = {} is not off|level|weapon|both; using off", progression);
			progression = "off";
		}
		auto pf = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Progression", a_key, a_value)); };
		pf("level_reference", levelReference);
		pf("level_exponent", levelExponent);
		pf("weapon_bonus", weaponBonus);
		pf("min_factor", progressionMin);
		pf("max_factor", progressionMax);

		// [TimeSkip] (er/TimeSkip.h)
		timeSkip = ini.GetBool("TimeSkip", "enabled", timeSkip);
		timeSkipSmoothMs = std::clamp(static_cast<int>(ini.GetInt("TimeSkip", "smooth_ms", timeSkipSmoothMs)), 0, 60000);
		timeSkipStepMs = std::clamp(static_cast<int>(ini.GetInt("TimeSkip", "step_ms", timeSkipStepMs)), 16, 5000);

		auto tf = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Throw", a_key, a_value)); };
		throwSim = ini.GetBool("Throw", "sim", throwSim);
		tf("gravity", throwGravity);
		tf("restitution", throwRestitution);
		tf("friction", throwFriction);
		tf("roll_decel", throwRollDecel);
		tf("impact_wait", throwImpactWait);
		tf("radius_lethal", throwRadiusLethal);
		tf("radius_sleep", throwRadiusSleep);
		tf("radius_stun", throwRadiusStun);
		tf("radius_smoke", throwRadiusSmoke);
		tf("ball_radius", throwBallRadius);
		throwArc = ini.Get("Throw", "arc", throwArc);
		if (throwArc != "auto" && throwArc != "off") {
			Log::Warn("[Throw] arc = {} is not auto|off; using auto", throwArc);
			throwArc = "auto";
		}
		tf("arc_spacing", throwArcSpacing);
		tf("arc_dot", throwArcDot);
		tf("arc_ring", throwArcRing);
		Floats(ini.Get("Throw", "arc_color"), throwArcColor, 4);

		mines = ini.GetBool("Mines", "enabled", mines);
		mineArmSeconds = static_cast<float>(ini.GetFloat("Mines", "arm_seconds", mineArmSeconds));
		mineBodyRadius = static_cast<float>(ini.GetFloat("Mines", "body_radius", mineBodyRadius));
		mineLightRadius = static_cast<float>(ini.GetFloat("Mines", "light_radius", mineLightRadius));

		{
			auto df = [&](const char* a_key, float& a_value) { a_value = static_cast<float>(ini.GetFloat("Decoys", a_key, a_value)); };
			decoys = ini.GetBool("Decoys", "enabled", decoys);
			df("range", decoyRange);
			df("active_range", decoyActiveRange);
			df("cone", decoyCone);
			df("stare", decoyStare);
			df("cooldown", decoyCooldown);
			df("pop_distance", decoyPopDistance);
			df("snake_close", decoySnakeClose);
			df("shock_radius", decoyShockRadius);
			df("height", decoyHeight);
			decoyTurn = ini.GetBool("Decoys", "turn", decoyTurn);
		}

		auto& r = render;
		r.enabled = ini.GetBool("Render", "enabled", r.enabled);
		r.drawPlayer = ini.GetBool("Render", "draw_player", r.drawPlayer);
		r.drawBuddy = ini.GetBool("Render", "draw_buddy", r.drawBuddy);
		r.drawOther = ini.GetBool("Render", "draw_other", r.drawOther);
		r.projectileMeshes = ini.GetBool("Render", "projectile_meshes", r.projectileMeshes);
		r.projectileSpin = static_cast<float>(ini.GetFloat("Render", "projectile_spin", r.projectileSpin));
		r.latencyFrames = static_cast<int>(ini.GetInt("Render", "latency_frames", r.latencyFrames));
		r.cull = ini.Get("Render", "cull", r.cull);
		r.frontCcw = ini.GetBool("Render", "front_ccw", r.frontCcw);
		r.flipX = ini.GetBool("Render", "flip_x", r.flipX);
		Floats(ini.Get("Render", "light_dir"), r.lightDir, 3);
		Floats(ini.Get("Render", "light_color"), r.lightColor, 3);
		r.lightIntensity = static_cast<float>(ini.GetFloat("Render", "light_intensity", r.lightIntensity));
		Floats(ini.Get("Render", "ambient_up"), r.ambientUp, 3);
		Floats(ini.Get("Render", "ambient_down"), r.ambientDown, 3);
		r.exposure = static_cast<float>(ini.GetFloat("Render", "exposure", r.exposure));
		r.dayNight = ini.GetBool("Render", "day_night", r.dayNight);
		r.albedoSrgb = ini.GetBool("Render", "albedo_srgb", r.albedoSrgb);
		r.normalXy = ini.Get("Render", "normal_xy", r.normalXy);
		r.hdrPaperWhite = static_cast<float>(ini.GetFloat("Render", "hdr_paper_white", r.hdrPaperWhite));
		r.debugView = static_cast<int>(ini.GetInt("Render", "debug_view", r.debugView));
		r.xray = static_cast<float>(ini.GetFloat("Render", "xray", r.xray));
		r.erDepth = ini.GetBool("Render", "er_depth", r.erDepth);
		r.erDepthBias = static_cast<float>(ini.GetFloat("Render", "er_depth_bias", r.erDepthBias));
		r.erDepthNear = static_cast<float>(ini.GetFloat("Render", "er_depth_near", r.erDepthNear));
		r.probe = ini.GetBool("Render", "probe", r.probe);
		r.stage = ini.Get("Render", "stage", r.stage);
		if (r.stage == "hdr") {
			Log::Warn("[Render] stage = hdr (stage B+) is not built; using gbuffer");
			r.stage = "gbuffer";
		}
		if (r.stage != "gbuffer" && r.stage != "post" && r.stage != "present") {
			Log::Warn("[Render] stage = {} is not gbuffer|post|present; using gbuffer", r.stage);
			r.stage = "gbuffer";
		}
		r.gbufferDepth = ini.Get("Render", "gbuffer_depth", r.gbufferDepth);
		r.gbufferStencil = static_cast<int>(ini.GetInt("Render", "gbuffer_stencil", r.gbufferStencil));
		if (const auto gp = ini.Get("Render", "gbuffer_point"); gp == "clear") {
			r.gbufferPoint = -1;
		} else if (gp == "last") {
			r.gbufferPoint = 0;
		} else {
			r.gbufferPoint = std::max(-1, static_cast<int>(ini.GetInt("Render", "gbuffer_point", r.gbufferPoint)));
		}
		if (const auto ls = ini.Get("Render", "gbuffer_light_set"); ls == "auto") {
			r.gbufferLightSet = -1;
		} else {
			r.gbufferLightSet = static_cast<int>(ini.GetInt("Render", "gbuffer_light_set", r.gbufferLightSet));
		}
		r.gbufferAo = static_cast<float>(ini.GetFloat("Render", "gbuffer_ao", r.gbufferAo));
		r.gbufferTranslucency = static_cast<float>(ini.GetFloat("Render", "gbuffer_translucency", r.gbufferTranslucency));
		r.albedoScale = static_cast<float>(ini.GetFloat("Render", "albedo_scale", r.albedoScale));
		r.glossScale = static_cast<float>(ini.GetFloat("Render", "gloss_scale", r.glossScale));
		r.glossBias = static_cast<float>(ini.GetFloat("Render", "gloss_bias", r.glossBias));
		r.metalScale = static_cast<float>(ini.GetFloat("Render", "metal_scale", r.metalScale));
		r.alphaTest = ini.Get("Render", "alpha_test", r.alphaTest);
		r.alphaRef = static_cast<float>(ini.GetFloat("Render", "alpha_ref", r.alphaRef));
		if (const auto text = ini.Get("Render", "camo_materials"); !text.empty()) {
			r.camoMaterials.clear();
			std::istringstream in(text);
			std::string        item;
			while (std::getline(in, item, ',')) {
				char*      end = nullptr;
				const auto v = std::strtoul(item.c_str(), &end, 16);
				if (end != item.c_str()) {
					r.camoMaterials.push_back(static_cast<std::uint32_t>(v));
				}
			}
		}
		if (const auto text = ini.Get("Render", "poster_materials"); !text.empty()) {
			r.posterMaterials.clear();
			std::istringstream in(text);
			std::string        item;
			while (std::getline(in, item, ',')) {
				char*      end = nullptr;
				const auto v = std::strtoul(item.c_str(), &end, 16);
				if (end != item.c_str()) {
					r.posterMaterials.push_back(static_cast<std::uint32_t>(v));
				}
			}
		}
		r.roleGuard = ini.Get("Render", "role_guard", r.roleGuard);
		r.posterCheck = ini.Get("Render", "poster_check", r.posterCheck);
		r.velocity = ini.GetBool("Render", "velocity", r.velocity);
		r.smoothDraws = ini.GetBool("Render", "smooth_draws", r.smoothDraws);
		r.viewConstants = ini.GetBool("Render", "view_constants", r.viewConstants);
		r.shadows = ini.GetBool("Render", "shadows", r.shadows);
		Floats(ini.Get("Render", "velocity_scale"), r.velocityScale, 2);
		Floats(ini.Get("Render", "velocity_bias"), r.velocityBias, 2);
		Floats(ini.Get("Render", "near_fade"), r.nearFade, 2);
		Floats(ini.Get("Render", "near_fade_other"), r.nearFadeOther, 2);
		r.occluderFade = static_cast<float>(ini.GetFloat("Render", "occluder_fade", r.occluderFade));
		r.occluderRadius = static_cast<float>(ini.GetFloat("Render", "occluder_radius", r.occluderRadius));
		r.uploadMbPerFrame = static_cast<int>(ini.GetInt("Render", "upload_mb_per_frame", r.uploadMbPerFrame));
		r.deathVisual = ini.Get("Render", "death_visual", r.deathVisual);
		if (r.deathVisual != "fade" && r.deathVisual != "keep" && r.deathVisual != "cut") {
			Log::Warn("[Render] death_visual = {} is not fade|keep|cut; using keep", r.deathVisual);
			r.deathVisual = "keep";
		}
		r.deathFadeMs = static_cast<int>(ini.GetInt("Render", "death_fade_ms", r.deathFadeMs));
		r.nvg = ini.GetBool("Render", "nvg", r.nvg);
		r.nvgGain = static_cast<float>(ini.GetFloat("Render", "nvg_gain", r.nvgGain));
		r.nvgCurve = static_cast<float>(ini.GetFloat("Render", "nvg_curve", r.nvgCurve));
		r.nvgFloor = static_cast<float>(ini.GetFloat("Render", "nvg_floor", r.nvgFloor));
		Floats(ini.Get("Render", "nvg_tint"), r.nvgTint, 3);
		r.nvgGrain = static_cast<float>(ini.GetFloat("Render", "nvg_grain", r.nvgGrain));
		r.nvgVignette = static_cast<float>(ini.GetFloat("Render", "nvg_vignette", r.nvgVignette));
		r.nvgScanlines = static_cast<float>(ini.GetFloat("Render", "nvg_scanlines", r.nvgScanlines));
		r.nvgFadeMs = static_cast<int>(ini.GetInt("Render", "nvg_fade_ms", r.nvgFadeMs));
		r.debugLayer = ini.GetBool("Render", "debug_layer", r.debugLayer);
		r.gpuValidation = ini.GetBool("Render", "gpu_validation", r.gpuValidation);

		auto& o = overlay;
		o.enabled = ini.GetBool("Overlay", "enabled", o.enabled);
		o.hud = ini.GetBool("Overlay", "hud", o.hud);
		o.hudFadeMs = static_cast<int>(ini.GetInt("Overlay", "hud_fade_ms", o.hudFadeMs));
		o.reticle = ini.Get("Overlay", "reticle", o.reticle);
		if (o.reticle != "auto" && o.reticle != "on" && o.reticle != "off") {
			Log::Warn("[Overlay] reticle = {} is not auto|on|off; using auto", o.reticle);
			o.reticle = "auto";
		}
		o.reticleScale = static_cast<float>(ini.GetFloat("Overlay", "reticle_scale", o.reticleScale));
		o.status = ini.GetBool("Overlay", "status", o.status);
		o.messages = ini.GetBool("Overlay", "messages", o.messages);
		o.messageSeconds = static_cast<float>(ini.GetFloat("Overlay", "message_seconds", o.messageSeconds));
		o.absentSeconds = static_cast<float>(ini.GetFloat("Overlay", "absent_seconds", o.absentSeconds));
		o.textScale = static_cast<float>(ini.GetFloat("Overlay", "text_scale", o.textScale));
		o.corner = ini.Get("Overlay", "corner", o.corner);
		o.hudScale = static_cast<float>(ini.GetFloat("Overlay", "hud_scale", o.hudScale));
		Floats(ini.Get("Overlay", "hud_offset"), o.hudOffset, 2);
		Floats(ini.Get("Overlay", "hud_weapon_region"), o.hudWeaponRegion, 4);
		Floats(ini.Get("Overlay", "hud_weapon_bar_region"), o.hudWeaponBarRegion, 4);
		Floats(ini.Get("Overlay", "hud_weapon_offset"), o.hudWeaponOffset, 2);
		Floats(ini.Get("Overlay", "hud_notify_region"), o.hudNotifyRegion, 4);
		Floats(ini.Get("Overlay", "hud_notify_offset"), o.hudNotifyOffset, 2);
		if (o.corner != "top_right" && o.corner != "top_left" && o.corner != "bottom_right" && o.corner != "bottom_left") {
			Log::Warn("[Overlay] corner = {} is not top_right|top_left|bottom_right|bottom_left; using top_right", o.corner);
			o.corner = "top_right";
		}
		keyboard.Load(ini);  // [Keyboard] (er/KbmMap.cpp)

		// [Marking] (er/Marking.h)
		auto& mk = marking;
		mk.enabled = ini.GetBool("Marking", "enabled", mk.enabled);
		mk.targets = ini.Get("Marking", "targets", mk.targets);
		if (mk.targets != "all" && mk.targets != "hostile") {
			Log::Warn("[Marking] targets = {} is not all|hostile; using all", mk.targets);
			mk.targets = "all";
		}
		mk.range = static_cast<float>(ini.GetFloat("Marking", "range", mk.range));
		mk.cone = static_cast<float>(ini.GetFloat("Marking", "cone", mk.cone));
		mk.minConeDeg = static_cast<float>(ini.GetFloat("Marking", "min_cone_deg", mk.minConeDeg));
		mk.dwell = static_cast<float>(ini.GetFloat("Marking", "dwell", mk.dwell));
		mk.lineOfSight = ini.GetBool("Marking", "line_of_sight", mk.lineOfSight);
		mk.renderedOnly = ini.GetBool("Marking", "rendered_only", mk.renderedOnly);
		mk.fade = static_cast<float>(ini.GetFloat("Marking", "fade", mk.fade));
		mk.maxMarked = static_cast<int>(ini.GetInt("Marking", "max_marked", mk.maxMarked));
		mk.maxCharacters = static_cast<int>(ini.GetInt("Marking", "max_characters", mk.maxCharacters));
		mk.distance = ini.Get("Marking", "distance", mk.distance);
		if (mk.distance != "always" && mk.distance != "centre" && mk.distance != "off") {
			Log::Warn("[Marking] distance = {} is not always|centre|off; using always", mk.distance);
			mk.distance = "always";
		}
		mk.size = static_cast<float>(ini.GetFloat("Marking", "size", mk.size));
		mk.textSize = static_cast<float>(ini.GetFloat("Marking", "text_size", mk.textSize));
		mk.height = static_cast<float>(ini.GetFloat("Marking", "height", mk.height));
		return found;
	}
}
