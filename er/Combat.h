#pragma once

// Snake hurting Elden Ring (DESIGN.md §8.1, docs/COMBAT.md). Guest events arrive at FrameBegin
// (er/Events.cpp) and are resolved here on the game's task thread:
//
//  kGEvFire / kGEvBuddyFire  muzzle + direction mapped through the anchor; ER's world ray gives the
//                            first wall, and a ray-vs-capsule test against every character nearby (feet
//                            + hit height/radius from its physics module) gives the first body before it
//  kGEvMelee                 the strike sphere MGSV reports, plus an arc in front of Snake; the nearest
//                            character it touches takes the hit
//  kGEvExplosion / explosive rounds   every character inside the blast, damage falling off with distance
//
// A hit is applied through Elden Ring's own pipeline when it can be:
//  - damage + poise: a bullet owned by the Tarnished spawned just in front of the hit point and aimed at
//    it (CSBulletManager::SpawnBullet), with its AtkParam_Pc row patched to this hit's damage, poise and
//    flinch level: the enemy flinches/staggers, turns on the player and takes damage after its own
//    defence (route bullet);
//  - or ChangeHp / Kill directly (route hp: always lands, no reaction, no aggro);
//  - route auto: bullet, verified per hit (did the target lose HP within verify_ms?) until one lands;
//    unlanded hits get the hp fallback meanwhile, and too many misses switch to hp for the session;
//  - sleep (tranquilizer, choke, sleep gas): rungs of Elden Ring's own sleep build-up ladder (25000..25019) on the hit
//    bullet's spEffectId slots, as its own sleep bullets carry them (applied directly they build nothing: docs/COMBAT.md).
// Only characters Elden Ring's team relation says the Tarnished may attack are hurt ([Combat] targets).

#include "CombatLogic.h"
#include "Host.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::er
{
	namespace Combat
	{
		void OnShot(const Frame& a_frame, const proto::GuestEvent& a_event, bool a_buddy);
		void OnMelee(const Frame& a_frame, const proto::GuestEvent& a_event);
		void OnExplosion(const Frame& a_frame, const proto::GuestEvent& a_event);
		// A blast of AmmoKind a_kind at an Elden Ring point (our thrown grenade, a placed mine): Elden Ring's pot effect
		// (er/Explosion.h) and the damage / sleep / stagger of kGEvExplosion. a_onGround: drop the effect onto the ground.
		void BlastAt(const Frame& a_frame, std::uint32_t a_kind, const Vec3& a_centre, float a_radius, bool a_onGround, std::string_view a_what);
		// Every character the Tarnished may attack (Elden Ring's team relation) whose capsule may come within a_radius of
		// a_centre: one scan of Elden Ring's characters for many tests (the mines test each of theirs against the list).
		struct Foe
		{
			combat::Capsule cap;
			std::string     name;
			std::uint8_t*   chr{ nullptr };
			std::uint64_t   handle{ 0 };
			Quat            orientation;
		};
		std::vector<Foe> HostilesNear(const Frame& a_frame, const Vec3& a_centre, float a_radius);
		// Is a character of that team hostile to the Tarnished (Elden Ring's team relation table, the rule every hit uses)?
		bool HostileToPlayer(const Frame& a_frame, std::uint8_t a_team);
		// Is the character asleep (Elden Ring's sleep status: a sleep build-up row, stateInfo 436, still on it next to the
		// "helpless" row, stateInfo 434, that comes with the proc; live 2026-10-14, a Stormveil soldier slept by our ladder:
		// 25019 (436) and 54 (434) from 0.9 s to 4.4 s, both gone when he woke at ~5 s; 434 alone is a stance break)?
		bool Asleep(std::uint8_t* a_chr);

		void Tick(const Frame& a_frame);  // FrameBegin, after the events: verifies pending bullet hits
		void OnPuppetStop();               // puts every patched param byte back

		// Dev console: `combat [status|route auto|bullet|hp|hit n [dmg [poise]]|sleep n [power]|team n|targets]`.
		std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
		std::string Describe();
	}
}
