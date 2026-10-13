#include "Events.h"

#include "BuddyOrders.h"
#include "Combat.h"
#include "Host.h"
#include "InputBridge.h"
#include "Link.h"
#include "Log.h"
#include "Mines.h"
#include "Puppet.h"
#include "Rounds.h"
#include "Stealth.h"
#include "Throw.h"

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
				Mines::OnPlaced(a_f, e);
				break;
			case proto::kGEvPlacedGone:
				Mines::OnGone(e);
				break;
			case proto::kGEvBuddyOrder:
				BuddyOrders::OnOrder(e);
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
	}

	unsigned long long Count() { return g_count; }
}
