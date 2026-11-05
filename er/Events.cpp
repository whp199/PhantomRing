#include "Events.h"

#include "Buddy.h"
#include "BuddyOrders.h"
#include "Combat.h"
#include "Cqc.h"
#include "Decoys.h"
#include "Dog.h"
#include "Fulton.h"
#include "GraceRest.h"
#include "Host.h"
#include "InputBridge.h"
#include "Link.h"
#include "Log.h"
#include "Marking.h"
#include "Mines.h"
#include "Noise.h"
#include "Puppet.h"
#include "Quiet.h"
#include "Rounds.h"
#include "Stealth.h"
#include "Throw.h"
#include "TimeSkip.h"

#include <cstdint>
#include <format>
#include <iterator>

namespace phantom::er::Events
{
	namespace
	{
		unsigned long long g_count{ 0 };
	}

	void Poll(const Frame& a_f)
	{
		proto::GuestEvent e{};
		for (int i = 0; i < 256 && Link::Get().PopGuestEvent(e); ++i) {
			++g_count;
			switch (e.type) {
			case proto::kGEvFire:
				Combat::OnShot(a_f, e, false);
				break;
			case proto::kGEvBuddyFire:
				Combat::OnShot(a_f, e, true);
				break;
			case proto::kGEvMelee:
				Combat::OnMelee(a_f, e);
				break;
			case proto::kGEvDied:
				Puppet::OnGuestDied(a_f);
				break;
			case proto::kGEvRumble:
				InputBridge::Rumble(static_cast<std::uint16_t>(e.a), static_cast<std::uint16_t>(e.b));
				break;
			case proto::kGEvExplosion:
				// Ours first: a thrown grenade we fly (er/Throw.h) or a mine we keep (er/Mines.h) blasts where OUR copy is.
				if (!Throw::OnExplosion(a_f, e) && !Mines::OnExplosion(a_f, e)) {
					Combat::OnExplosion(a_f, e);
				}
				break;
			case proto::kGEvThrow:
				Throw::OnThrow(a_f, e);
				break;
			case proto::kGEvPlaced:
				if (e.a == proto::kAmmoDecoy) {
					Decoys::OnPlaced(a_f, e);  // (v13) a decoy: stared at, never set off (er/Decoys.h)
				} else {
					Mines::OnPlaced(a_f, e);
				}
				break;
			case proto::kGEvPlacedGone:
				Mines::OnGone(e);
				Decoys::OnGone(e);  // ids are shared: each drops only its own
				break;
			case proto::kGEvBuddyOrder:
				BuddyOrders::OnOrder(e);
				Buddy::OnOrder(e.a, e.b);  // (0.8.0 polish) D-Dog's Wait pinned to its Elden Ring spot
				Dog::OnOrder(a_f, e);      // (v17) D-Dog's style (pad[0]) and Bark
				Quiet::OnOrder(a_f, e);    // (v18) Quiet's orders (b = 3)
				break;
			case proto::kGEvFulton:
				Fulton::OnEvent(a_f, e);
				break;
			case proto::kGEvTimeSkip:
				TimeSkip::OnEvent(e);  // MGSV's Phantom Cigar (er/TimeSkip.h)
				break;
			case proto::kGEvCqc:
				Cqc::OnEvent(a_f, e);  // (v13) CQC holds and throws (er/Cqc.h)
				break;
			case proto::kGEvDogAttack:
				Dog::OnAttack(a_f, e);  // (v17) D-Dog's attack on an Elden Ring target (er/Dog.h; only logged with [Dog] enabled = false)
				Buddy::OnDogAttack(static_cast<std::uint32_t>(proto::DogAttackPhase(e.a)));  // an order taken from Wait ends the pin
				break;
			case proto::kGEvQuietShot:
				Quiet::OnShot(a_f, e);  // (v18) Quiet's shot at an Elden Ring target (er/Quiet.h; only logged with [Quiet] enabled = false)
				break;
			case proto::kGEvLog:
				Log::Info("guest message {}", e.a);
				break;
			default:
				Log::Debug("guest event type {} ignored", e.type);
				break;
			}
		}
		Combat::Tick(a_f);
		Throw::Tick(a_f);
		Rounds::Tick(a_f);
		Stealth::Tick(a_f);
		Mines::Tick(a_f);
		Decoys::Tick(a_f);
		Noise::Tick(a_f);  // (v16) "Suppressor worn out"
		Fulton::Tick(a_f);
		Marking::Tick(a_f);  // protocol v13 kGuestBinoculars
		TimeSkip::Tick(a_f);
		Cqc::Tick(a_f);
		Dog::Tick(a_f);  // (v17) D-Dog: spotting, the pick, its holds and stares (after Cqc: a hold that just ended is seen)
		GraceRest::Tick(a_f);  // (v18) the rest's observed black, the seat and the guest's echo (before Quiet: her place follows it)
		Quiet::Tick(a_f);      // (v18) Quiet: her post, the pick, spotting, the reticle
	}

	unsigned long long Count() { return g_count; }
}
