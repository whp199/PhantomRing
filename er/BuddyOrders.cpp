#include "BuddyOrders.h"

#include "Config.h"
#include "Host.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Log.h"
#include "Puppet.h"
#include "QuietLogic.h"

#include <format>

#include <windows.h>

namespace phantom::er::BuddyOrders
{
	namespace
	{
		constexpr std::uint16_t kLb = 0x0100;  // XINPUT_GAMEPAD_LEFT_SHOULDER: MGSV's call menu while held
		constexpr std::uint32_t kWalker = 4;   // BuddyType WALKER_GEAR
		constexpr std::uint32_t kDog = 2;      // BuddyType DOG
		constexpr std::uint32_t kQuiet = 3;    // BuddyType QUIET

		Hint          g_hint;
		std::uint32_t g_orders{ 0 };       // D-Walker orders MGSV took this session
		std::uint32_t g_dogOrders{ 0 };    // D-Dog's
		std::uint32_t g_quietOrders{ 0 };  // (v18) Quiet's
		std::uint32_t g_otherOrders{ 0 };  // other buddies' (not this mod's buddy, but shown)
		std::uint32_t g_howTos{ 0 }, g_noOrders{ 0 };
		std::uint32_t g_lastCommand{ 0 };
	}

	std::string OrderText(std::uint32_t a_command, std::uint32_t a_buddyType, bool a_dogTexts, bool a_quietTexts, std::uint32_t a_pad0)
	{
		// (v18) Quiet's (docs/research/quiet.md §3.3): the call menu's labels with what each does in Elden Ring.
		if (a_buddyType == kQuiet && a_quietTexts) {
			const auto weapon = a_pad0 >= 1 && a_pad0 <= 3 ? static_cast<proto::QuietWeapon>(a_pad0 - 1) : proto::kQuietWeaponUnknown;
			return quiet::OrderText(a_command, weapon);
		}
		// D-Dog's (docs/research/ddog.md §2.1, §3.2): the call menu's labels, with what each does in Elden Ring. 11-15 are one
		// order in MGSV (the dog's equipment decides what it does); the label the player clicked names it.
		if (a_buddyType == kDog && a_dogTexts) {
			switch (a_command) {
			case 8:
				return "D-Dog: Whistle -- he comes to you";
			case 9:
				return "D-Dog: Wait -- he stays where he is";
			case 10:
				return "D-Dog: Bark -- enemies near him stop and stare at him";
			case 11:
				return "D-Dog: Keep 'em busy -- he pins the enemy you look at";
			case 12:
				return "D-Dog: Do it (kill enemy) -- the enemy you look at";
			case 13:
				return "D-Dog: Do it (knock out enemy) -- he puts the enemy you look at to sleep";
			case 14:
				return "D-Dog: Do it (wound enemy) -- he cuts the enemy you look at down";
			case 15:
				return "D-Dog: Get it -- he fultons the sleeping enemy you look at";
			case 16:
				return "D-Dog: DD -- he comes over";
			default:
				return std::format("D-Dog: order {}", a_command);
			}
		}
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

	std::string HowToText(std::uint32_t a_buddyType)
	{
		// MGSV takes the click only with the stick back at the centre (VERIFIED live: 2 of 2 clicks with the stick still
		// pushed did nothing, 4 of 4 after letting it go gave the order); the entry stays selected meanwhile.
		switch (a_buddyType) {
		case kWalker:
			return "D-Walker orders: keep LB held, point the right stick at one, let the stick go, then click it in";
		case kDog:
			return "D-Dog orders: keep LB held, point the right stick at one, let the stick go, then click it in -- look at the enemy first";
		case kQuiet:
			return "Quiet orders: mark the enemy through your binoculars, then keep LB held, point the right stick at Fire, let it go and click it in";
		default:
			return {};
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
		(a_e.b == kWalker ? g_orders : a_e.b == kDog ? g_dogOrders : a_e.b == kQuiet ? g_quietOrders : g_otherOrders) += 1;
		const auto text = OrderText(a_e.a, a_e.b, Cfg().dog.orderText, Cfg().quiet.enabled && Cfg().quiet.orderText, a_e.pad[0]);
		// (v17) pad[0] = D-Dog's style + 1 (its equipment: what its commands 11-15 do), 0 = unknown / another buddy
		// (v18) b == 3 (Quiet, commands 1-3): pad[0] = QuietWeapon + 1, 0 = unknown / another command
		Log::Info("buddy: the player's order {} -- \"{}\"{}", a_e.a, text,
			a_e.b == 2 && a_e.pad[0]   ? std::format(" (D-Dog style {})", a_e.pad[0] - 1)
			: a_e.b == 3 && a_e.pad[0] ? std::format(" (Quiet weapon {})", a_e.pad[0] - 1)
			                           : std::string());
		Hud::Notify(text, Hud::Tone::kGood);
	}

	void Tick(const Frame& a_f)
	{
		const auto& g = a_f.guest;
		// The walker (or D-Dog, with [Dog] order_text) is out and Snake on foot, playing: LB is MGSV's call menu then.
		const bool buddy = g.buddyType == kWalker || (g.buddyType == kDog && Cfg().dog.orderText) ||
		                   (g.buddyType == kQuiet && Cfg().quiet.enabled && Cfg().quiet.orderText);
		const bool eligible = Puppet::Active() && a_f.guestReady && !a_f.menuOpen && !a_f.cutscene && !a_f.dead && !a_f.dying &&
		                      !(g.flags & (proto::kGuestInMenu | proto::kGuestOnVehicle)) && buddy && (g.buddyFlags & proto::kBuddyPresent);
		const bool lb = (InputBridge::LastButtons() & kLb) != 0;
		const float rx = static_cast<float>(InputBridge::LastRightStickX()) / 32767.0f;
		switch (g_hint.Update(::GetTickCount64(), eligible, lb, rx, g.buddyType == kDog ? g_dogOrders : g.buddyType == kQuiet ? g_quietOrders : g_orders)) {
		case Hint::Say::kHowTo:
			++g_howTos;
			Hud::Notify(HowToText(g.buddyType), Hud::Tone::kInfo);
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
		return std::format("orders: {} D-Walker, {} D-Dog, {} Quiet ({} other buddies), last command {}; hints: {} how-to, {} without an order", g_orders,
			g_dogOrders, g_quietOrders, g_otherOrders, g_lastCommand, g_howTos, g_noOrders);
	}
}
