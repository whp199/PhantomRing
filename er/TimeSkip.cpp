#include "TimeSkip.h"

#include "Addresses.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Hud.h"
#include "Log.h"
#include "Mem.h"
#include "TimeSkipLogic.h"
#include "../protocol/phantomring_protocol.h"

#include <windows.h>

#include <charconv>
#include <cmath>
#include <format>
#include <mutex>

namespace phantom::er::TimeSkip
{
	namespace
	{
		namespace ts = phantom::timeskip;

		// WorldAreaTime's time request: u32 hour, minute, second (docs/HOST.md "Lighting from Elden Ring"; the hour last).
		constexpr std::size_t kHour = 0x28, kMinute = 0x2C, kSecond = 0x30;
		// A session (one smoke) is over once nothing was pending and nothing written for this long: then the cursor is dropped
		// (Elden Ring's own clock is read afresh next time) and the corner says what passed.
		constexpr std::uint64_t kSessionEndMs = 3000;

		std::mutex    g_lock;
		ts::Pacer     g_pacer;
		bool          g_cursorValid{ false };
		std::int32_t  g_cursor{ 0 };
		std::uint64_t g_lastWriteMs{ 0 };
		bool          g_session{ false };
		std::int32_t  g_sessionFrom{ 0 };   // Elden Ring's clock at the first write
		std::int64_t  g_sessionSec{ 0 };    // written this session
		std::uint64_t g_sessionWrites{ 0 }; // writes this session (g_writes counts them all, for Describe)
		std::uint64_t g_events{ 0 }, g_minutes{ 0 }, g_writes{ 0 }, g_failedWrites{ 0 }, g_ignored{ 0 };

		std::uint64_t NowMs() { return ::GetTickCount64(); }

		ts::Pacer::Params PacerParams()
		{
			const auto& c = Cfg();
			ts::Pacer::Params p;
			p.smoothMs = static_cast<std::uint32_t>(std::max(c.timeSkipSmoothMs, 0));
			p.stepMs = static_cast<std::uint32_t>(std::max(c.timeSkipStepMs, 1));
			if (p.smoothMs == 0) {
				p.smoothMs = 1;
				p.minChunkSec = ts::kDay;  // at once: the whole pending skip in one write
			}
			return p;
		}

		// Elden Ring's clock as seconds of the day, or -1 (no world: a load, the title).
		std::int32_t ReadClock()
		{
			const float h = game::TimeOfDay();
			if (!std::isfinite(h)) {
				return -1;
			}
			return ts::Wrap(std::lround(static_cast<double>(h) * 3600.0));
		}

		bool WriteClock(const ts::Hms& a_t)
		{
			auto* time = Addr().Instance(A::WorldAreaTime);
			if (!time) {
				return false;
			}
			// Minute and second first: the hour write is what applies the three (VERIFIED live with `poke`).
			return SafeWrite<std::uint32_t>(time + kMinute, a_t.m) && SafeWrite<std::uint32_t>(time + kSecond, a_t.s) &&
			       SafeWrite<std::uint32_t>(time + kHour, a_t.h);
		}

		std::string Clock(std::int32_t a_sec)
		{
			const auto t = ts::ToHms(a_sec);
			return std::format("{:02}:{:02}", t.h, t.m);
		}

		std::string SpanText(std::uint64_t a_minutes)
		{
			const auto s = ts::SpanOf(a_minutes);
			return s.hours ? std::format("{} h {:02} min", s.hours, s.minutes) : std::format("{} min", s.minutes);
		}

		void Queue(std::uint32_t a_minutes, const char* a_from)
		{
			std::scoped_lock lock(g_lock);
			++g_events;
			g_minutes += a_minutes;
			g_pacer.Add(static_cast<std::int64_t>(a_minutes) * 60);
			Log::Debug("time skip: +{} min from {} ({} s pending)", a_minutes, a_from, g_pacer.Pending());
		}

		void EndSession(std::uint64_t a_now)
		{
			if (g_session && g_sessionSec > 0) {
				const auto minutes = static_cast<std::uint64_t>((g_sessionSec + 30) / 60);
				const auto to = ts::Wrap(static_cast<std::int64_t>(g_sessionFrom) + g_sessionSec);
				Log::Info("time skip: Elden Ring's clock {} -> {} (+{}, {} writes)", Clock(g_sessionFrom), Clock(to), SpanText(minutes), g_sessionWrites);
				Hud::Notify(std::format("Phantom Cigar: {} passed ({} -> {})", SpanText(minutes), Clock(g_sessionFrom), Clock(to)), Hud::Tone::kInfo);
			}
			g_session = false;
			g_sessionSec = 0;
			g_sessionWrites = 0;
			g_cursorValid = false;
			(void)a_now;
		}
	}

	void OnEvent(const proto::GuestEvent& a_event)
	{
		if (!Cfg().timeSkip) {
			std::scoped_lock lock(g_lock);
			if (g_ignored++ == 0) {
				Log::Info("time skip: MGSV's Phantom Cigar ran +{} min; [TimeSkip] enabled = false leaves Elden Ring's clock alone", a_event.a);
			}
			return;
		}
		// A cigar skips hours, never days at a time in one event (the guest sends every quarter second): anything bigger is junk.
		if (a_event.a == 0 || a_event.a > 24 * 60) {
			Log::Warn("time skip: kGEvTimeSkip with {} min ignored", a_event.a);
			return;
		}
		Queue(a_event.a, "MGSV");
	}

	void Tick(const Frame& a_frame)
	{
		std::scoped_lock lock(g_lock);
		const auto now = NowMs();
		if (g_pacer.Pending() <= 0) {
			if (g_session && now - g_lastWriteMs >= kSessionEndMs) {
				EndSession(now);
			}
			return;
		}
		// Only into a loaded world: during a load (or at the title) WorldAreaTime isn't the world's clock; the skip waits.
		if (!a_frame.inGame || a_frame.loading) {
			return;
		}
		const auto read = ReadClock();
		if (read < 0) {
			return;
		}
		const auto step = g_pacer.Step(now, PacerParams());
		if (step <= 0) {
			return;
		}
		if (!g_cursorValid) {
			g_cursor = read;
			g_cursorValid = true;
		} else {
			g_cursor = ts::Reconcile(g_cursor, read);
		}
		if (!g_session) {
			g_session = true;
			g_sessionFrom = g_cursor;
			g_sessionSec = 0;
			g_sessionWrites = 0;
			Log::Info("time skip: MGSV's Phantom Cigar -- Elden Ring's clock from {} on", Clock(g_cursor));
		}
		const auto next = ts::Advance(ts::ToHms(g_cursor), step);
		if (!WriteClock(next)) {
			if (g_failedWrites++ < 5) {
				Log::Warn("time skip: WorldAreaTime not writable; {} s dropped", step);
			}
			return;
		}
		g_cursor = ts::FromHms(next);
		g_sessionSec += step;
		g_lastWriteMs = now;
		++g_writes;
		++g_sessionWrites;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		const auto read = ReadClock();
		return std::format("time skip: {} | Elden Ring's clock {} | {} events, {} min received, {} s pending, {} writes ({} failed){}",
			Cfg().timeSkip ? "on" : "off ([TimeSkip] enabled = false)", read < 0 ? std::string("unknown") : Clock(read), g_events, g_minutes,
			g_pacer.Pending(), g_writes, g_failedWrites, g_session ? std::format(" | this skip: from {}, +{} s so far", Clock(g_sessionFrom), g_sessionSec) : std::string());
	}

	std::string Command(std::string_view a_args)
	{
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (a_args.empty() || a_args == "status") {
			return Describe();
		}
		if (a_args == "clear") {
			std::scoped_lock lock(g_lock);
			g_pacer.Clear();
			return "time skip: pending skip dropped";
		}
		unsigned   minutes = 0;
		const auto r = std::from_chars(a_args.data(), a_args.data() + a_args.size(), minutes);
		if (r.ec != std::errc() || minutes == 0 || minutes > 24 * 60) {
			return "timeskip [status | <minutes 1..1440> | clear]";
		}
		if (!Cfg().timeSkip) {
			return "time skip: [TimeSkip] enabled = false";
		}
		Queue(minutes, "the console");
		return std::format("time skip: +{} queued (as kGEvTimeSkip from MGSV's Phantom Cigar)", SpanText(minutes));
	}
}
