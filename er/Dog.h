#pragma once

// D-Dog as a battle buddy in Elden Ring (docs/research/ddog.md; protocol v17 kActorDogTarget / kGEvDogAttack / kBuddyEngaged;
// [Dog] in PhantomRingHost.ini). MGSV runs the dog (its follow, its run, leap and bite on a stand-in soldier at the Elden Ring
// enemy's place: the guest, mgsv/Dog.cpp); the host does what only Elden Ring can:
//  - spotting: provisional marks (er/Marking.h, hollow) on the hostile Elden Ring characters the dog smells -- within spot_range m
//    of it (spot_height m above or below), no line of sight -- and "D-Dog: Found enemy" in the corner;
//  - the pick: the one hostile enemy its next attack / fetch order would take (kActorDogTarget on his ActorTable record; the guest
//    reads it when the player gives the order): er/DogLogic.h Pick -- hostile by both of Elden Ring's team reads, alive, rendered,
//    3.5-25 m from Snake, near the view's centre, in sight, a clear straight run from the dog on Elden Ring's ground, never a boss
//    (bosses = off), never one CQC or a Fulton balloon has. A non-hostile character -- a merchant, a quest character, a spirit
//    summon -- is never a candidate. While LB (MGSV's call menu) is held, a paw over him (er/RenderMarkers.h owner kDog);
//  - the bite: kGEvDogAttack Connected applies the dog's style on that enemy through the usual hit route (er/Combat.h
//    HitCharacter: a bullet of the Tarnished's with this hit's numbers): busy = held (er/Cqc.h HoldFor) with a flinch now and
//    every busy_flinch_every s, kill = MGSV's knife share as damage + a knockdown, knock out = the sleep darts' build-up + a
//    knockdown, wound = a hit down to 1 HP at most + a knockdown, fetch = nothing (MGSV's Fulton arrives as kGEvFulton).
//    Re-checked when it lands: the handle must still be a living hostile non-boss Elden Ring enemy, else nothing happens;
//  - Bark (command 10): awake hostile non-boss enemies within bark_range m of the dog that aren't busy with Snake stop and stare
//    at it bark_stare s (ChrIns+0x538 no attack / no move, only our bits put back: the decoys' stare).
// Elden Ring's enemies don't attack the dog (v1: it is no Elden Ring character). On by default since the 0.8.0 polish; nothing at all
// runs while D-Dog isn't out. [Dog] enabled = false: kGEvDogAttack is only logged, nothing is picked, marked or applied.
// (0.8.0 polish) The pick shown is kept through the click (er/DogLogic.h PickLatch, the click grace); D-Dog's place and drawn body are
// guarded against one-frame jumps and a waiting dog keeps its Elden Ring spot (er/BuddyGuard.h, er/Buddy.cpp, er/RenderScene.cpp).

#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;

	namespace Dog
	{
		void OnAttack(const Frame& a_frame, const proto::GuestEvent& a_event);  // kGEvDogAttack (er/Events.cpp)
		void OnOrder(const Frame& a_frame, const proto::GuestEvent& a_event);   // kGEvBuddyOrder with b = 2 (D-Dog): style, Bark
		void Tick(const Frame& a_frame);  // FrameBegin, after the events: spotting, the pick, the paw, holds' flinches, stares
		void OnPuppetStop();
		// The ActorTable record that carries kActorDogTarget (er/Actors.cpp; 0 = none). Any thread.
		std::uint64_t TargetHandle();
		// Dev console: `dog [status | on | off | pick | test boss | attack <style> [0xHANDLE] | release | bark | set <key> <value>]`.
		std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
		std::string Describe();
	}
}
