#include "BuddyOrders.h"

#include "Host.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Log.h"
#include "Puppet.h"

#include <format>

#include <windows.h>

namespace phantom::er::BuddyOrders
{
	namespace
	{
		constexpr std::uint16_t kLb = 0x0100;  // XINPUT_GAMEPAD_LEFT_SHOULDER: MGSV's call menu while held
		constexpr std::uint32_t kWalker = 4;   // BuddyType WALKER_GEAR

		Hint          g_hint;
		std::uint32_t g_orders{ 0 };       // D-Walker orders MGSV took this session
		std::uint32_t g_otherOrders{ 0 };  // other buddies' (not this mod's buddy, but shown)
		std::uint32_t g_howTos{ 0 }, g_noOrders{ 0 };
		std::uint32_t g_lastCommand{ 0 };
	}

	std::string OrderText(std::uint32_t a_command, std::uint32_t a_buddyType)
	{
		// The call menu's own labels (docs/GUEST.md 18.3), with what each does for a player who never read MGSV's manual.
		switch (a_command) {
		case 20:
			return "D-Walker: Whistle -- it comes to you";
		case 21:
			return "D-Walker: Normal Mode -- it follows you";
		case 24:
			return "D-Walker: Stealth Mode -- it waits here, camouflaged";
		case 25:
			return "D-Walker: Search Mode -- it marks enemies nearby";
		case 26:
			return "D-Walker: Intercept Mode -- it attacks enemies nearby";
		default:
			return a_buddyType == kWalker ? std::format("D-Walker: order {}", a_command) : std::format("Buddy order {}", a_command);
		}
	}

	Hint::Say Hint::Update(std::uint64_t a_nowMs, bool a_eligible, bool a_lb, float a_rx, std::uint32_t a_orders)
	{
		if (!a_eligible) {
			down_ = waiting_ = false;
			return Say::kNothing;
		}
		if (waiting_ && a_orders != ordersAtDown_) {
			waiting_ = false;  // the order came: MGSV took it
		}
		if (a_lb && !down_) {
			down_ = true;
			howToShown_ = pointed_ = waiting_ = false;
			downAt_ = a_nowMs;
			ordersAtDown_ = a_orders;
		}
		if (a_lb) {
			pointed_ = pointed_ || a_rx > 0.5f;  // Whistle up-right, the modes right and down-right
			if (!howToShown_ && a_nowMs - downAt_ >= kHoldMs && a_orders < kLearnedAfter) {
				howToShown_ = true;
				return Say::kHowTo;
			}
			return Say::kNothing;
		}
		if (down_) {
			down_ = false;
			if (a_nowMs - downAt_ >= kHoldMs && pointed_ && a_orders == ordersAtDown_) {
				waiting_ = true;
				upAt_ = a_nowMs;
			}
		}
		if (waiting_ && a_nowMs - upAt_ >= kAnswerMs) {
			waiting_ = false;
			return Say::kNoOrder;
		}
		return Say::kNothing;
	}

	void OnOrder(const proto::GuestEvent& a_e)
	{
		g_lastCommand = a_e.a;
		(a_e.b == kWalker ? g_orders : g_otherOrders) += 1;
		const auto text = OrderText(a_e.a, a_e.b);
		Log::Info("buddy: the player's order {} -- \"{}\"", a_e.a, text);
		Hud::Notify(text, Hud::Tone::kGood);
	}

	void Tick(const Frame& a_f)
	{
		const auto& g = a_f.guest;
		// The walker is out and Snake on foot, playing: LB is MGSV's call menu then.
		const bool eligible = Puppet::Active() && a_f.guestReady && !a_f.menuOpen && !a_f.cutscene && !a_f.dead && !a_f.dying &&
		                      !(g.flags & (proto::kGuestInMenu | proto::kGuestOnVehicle)) && g.buddyType == kWalker && (g.buddyFlags & proto::kBuddyPresent);
		const bool lb = (InputBridge::LastButtons() & kLb) != 0;
		const float rx = static_cast<float>(InputBridge::LastRightStickX()) / 32767.0f;
		switch (g_hint.Update(::GetTickCount64(), eligible, lb, rx, g_orders)) {
		case Hint::Say::kHowTo:
			++g_howTos;
			// MGSV takes the click only with the stick back at the centre (VERIFIED live: 2 of 2 clicks with the stick still
			// pushed did nothing, 4 of 4 after letting it go gave the order); the entry stays selected meanwhile.
			Hud::Notify("D-Walker orders: keep LB held, point the right stick at one, let the stick go, then click it in", Hud::Tone::kInfo);
			break;
		case Hint::Say::kNoOrder:
			++g_noOrders;
			Log::Info("buddy: LB menu closed without an order (no right-stick click)");
			Hud::Notify("No order given -- let the right stick go, then click it in while LB is still held", Hud::Tone::kWarn);
			break;
		default:
			break;
		}
	}

	std::string Describe()
	{
		return std::format("orders: {} D-Walker ({} other buddies), last command {}; hints: {} how-to, {} without an order", g_orders, g_otherOrders, g_lastCommand,
			g_howTos, g_noOrders);
	}
}
