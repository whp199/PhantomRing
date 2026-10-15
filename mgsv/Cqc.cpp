#include "Cqc.h"

#include "Combat.h"
#include "Config.h"
#include "CqcLogic.h"
#include "Death.h"
#include "Link.h"
#include "Log.h"
#include "Menu.h"
#include "State.h"

#include "phantomring_protocol.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>

namespace phantom::guest::cqc
{
	namespace
	{
		// What stops MGSV from doing CQC at all: a menu or cutscene, death, riding, carrying a body (RT throws the body), the box
		// (RT is the box's), prone, a fall, first person / binoculars.
		constexpr std::uint32_t kNoCqc = state::kLMenu | state::kLDemo | state::kLDead | state::kLVehicle | state::kLCarry | state::kLCbox |
		                                 state::kLCrawl | state::kLFall | state::kLSubject | state::kLBinocle;
		constexpr int kButtonLogBudget = 64;  // button changes logged during holds per session (the live mapping of MGSV's controls)

		Tracker  g_tracker;
		// Snake's last place and the counts, for the console (another thread).
		std::mutex                 g_mutex;
		float                      g_root[3]{};
		float                      g_yaw{ 0 };
		std::uint64_t              g_frame{ 0 };
		std::array<std::uint64_t, 6> g_sent{};
		std::uint32_t              g_lastButtons{ 0 };
		std::atomic<bool>          g_lastMgsv{ false };
		int                        g_buttonLogs{ 0 };
		std::string                g_last;
		Tracker                    g_shown;  // the tracker as of the last frame (the console reads this copy)

		Rules MakeRules()
		{
			const auto& c = Cfg();
			Rules       r;
			r.cqcMask = c.cqcButtons;
			r.aimMask = c.cqcAimButtons;
			r.interrogateMask = c.cqcInterrogateButtons;
			r.chokeMask = c.cqcChokeButtons;
			r.holdMs = c.cqcHoldMs;
			r.chokeMs = c.cqcChokeMs;
			r.maxHoldMs = c.cqcMaxHoldMs;
			r.cooldownMs = c.cqcCooldownMs;
			r.throwStick = c.cqcThrowStick;
			return r;
		}

		void Send(proto::CqcAction a_action, const float a_root[3], float a_yaw, std::uint64_t a_frame, std::uint16_t a_attack, std::string_view a_why)
		{
			proto::GuestEvent e{};
			e.type = proto::kGEvCqc;
			e.a = a_action;
			e.b = static_cast<std::uint32_t>(Cfg().cqcRange * 1000.0f);
			e.frame = static_cast<std::uint32_t>(a_frame);
			e.v0[0] = a_root[0];
			e.v0[1] = a_root[1];
			e.v0[2] = a_root[2];
			e.v1[0] = std::sin(a_yaw);
			e.v1[2] = std::cos(a_yaw);
			e.pad[0] = a_attack;
			e.pad[1] = 0;
			const bool sent = link::PushEvent(e);
			std::lock_guard lock(g_mutex);
			if (a_action < g_sent.size()) {
				++g_sent[a_action];
			}
			g_last = std::format("{} ({}) at {:.2f},{:.2f},{:.2f}", Name(a_action), a_why, a_root[0], a_root[1], a_root[2]);
			Log::Info("cqc: kGEvCqc {} ({}; MGSV attack {}) at {:.2f},{:.2f},{:.2f}{}", Name(a_action), a_why, a_attack, a_root[0], a_root[1], a_root[2],
				sent ? "" : " -- NOT sent (event ring full or detached)");
		}
	}

	void Update(const FrameInput& a_in)
	{
		const auto& c = Cfg();
		if (!c.cqc) {
			g_tracker.Reset();
			return;
		}
		const auto  probe = combat::Probe();
		// Only while playing: PollMelee (the probe) runs only then, and a stale probe must not keep "watching" on.
		const bool  mgsvHold = a_in.ready && ((a_in.luaFlags & state::kLCqc) != 0 || (probe.valid && probe.action));
		proto::PadState pad{};
		const float stick = link::ReadPad(pad) ? StickDeflection(pad.thumbLX, pad.thumbLY) : 0.0f;
		Input       in;
		in.allowed = a_in.ready && !a_in.riding && (a_in.luaFlags & kNoCqc) == 0 && !death::Down() && !menu::PauseOpen();
		in.buttons = a_in.buttons;
		in.stick = stick;
		in.mgsvHold = c.cqcMgsvHold && mgsvHold;
		in.nowMs = ::GetTickCount64();
		const auto before = g_tracker.Current();
		const auto actions = g_tracker.Update(in, MakeRules());
		{
			std::lock_guard lock(g_mutex);
			std::copy_n(a_in.root, 3, g_root);
			g_yaw = a_in.yaw;
			g_frame = a_in.frame;
			g_shown = g_tracker;
		}
		// The live mapping of MGSV's controls: while RT is down (or MGSV holds someone), every change of its pad bits and of its own
		// CQC signals, so a hold on a real soldier (an ActorProxy) shows which bits interrogate and choke.
		const bool watching = before != Tracker::State::kIdle || g_tracker.Current() != Tracker::State::kIdle || mgsvHold;
		if (watching && (a_in.buttons != g_lastButtons || mgsvHold != g_lastMgsv.load()) && g_buttonLogs < kButtonLogBudget) {
			++g_buttonLogs;
			Log::Info("cqc: [{}] pad bits {:#x} -> {:#x}, stick {:.2f}, MGSV CQC status {} action callback {} attack {}", Name(g_tracker.Current()), g_lastButtons,
				a_in.buttons, stick, (a_in.luaFlags & state::kLCqc) ? 1 : 0, probe.valid && probe.action ? 1 : 0, probe.attack);
		}
		g_lastButtons = a_in.buttons;
		g_lastMgsv = mgsvHold;
		for (std::size_t i = 0; i < actions.count; ++i) {
			const auto a = actions.list[i];
			const char* why = a == proto::kCqcThrow                                 ? "RT with the stick pushed"
			                  : a == proto::kCqcGrab                                ? (in.mgsvHold ? "MGSV holds" : "RT held")
			                  : a == proto::kCqcInterrogate                         ? "interrogate button"
			                  : a == proto::kCqcChoke                               ? ((a_in.buttons & c.cqcChokeButtons) != 0 ? "choke button" : "RT held on")
			                  : !in.allowed                                         ? "CQC no longer possible"
			                                                                        : "let go";
			Send(a, a_in.root, a_in.yaw, a_in.frame, probe.valid ? probe.attack : 0, why);
		}
	}

	std::string Console(std::string_view a_args)
	{
		static constexpr std::array<std::pair<std::string_view, proto::CqcAction>, 6> kNames{ {
			{ "grab", proto::kCqcGrab },
			{ "hold", proto::kCqcHold },
			{ "throw", proto::kCqcThrow },
			{ "choke", proto::kCqcChoke },
			{ "interrogate", proto::kCqcInterrogate },
			{ "release", proto::kCqcRelease },
		} };
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (!a_args.empty()) {
			for (const auto& [name, action] : kNames) {
				if (a_args == name) {
					float         root[3];
					float         yaw;
					std::uint64_t frame;
					{
						std::lock_guard lock(g_mutex);
						std::copy_n(g_root, 3, root);
						yaw = g_yaw;
						frame = g_frame;
					}
					Send(action, root, yaw, frame, 0, "console");
					return std::format("sent kGEvCqc {}", name);
				}
			}
			return "usage: !cqc [grab|hold|throw|choke|interrogate|release]";
		}
		std::lock_guard lock(g_mutex);
		return std::format("cqc: {} ({}, MGSV hold {}); tracker {}{}{}; sent grab {} throw {} choke {} interrogate {} release {}; last: {}",
			Cfg().cqc ? "on" : "off", Cfg().cqcMgsvHold ? "starts grabs" : "logged only", g_lastMgsv.load() ? "now" : "no", Name(g_shown.Current()),
			g_shown.Choked() ? ", choked" : "", g_shown.Interrogated() ? ", interrogated" : "", g_sent[proto::kCqcGrab], g_sent[proto::kCqcThrow],
			g_sent[proto::kCqcChoke], g_sent[proto::kCqcInterrogate], g_sent[proto::kCqcRelease], g_last.empty() ? "none" : g_last);
	}
}
