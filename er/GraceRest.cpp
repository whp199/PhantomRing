#include "GraceRest.h"

#include "Addresses.h"
#include "Anchor.h"
#include "Buddy.h"
#include "Calibration.h"
#include "Config.h"
#include "Game.h"
#include "GeomProbe.h"
#include "GraceLogic.h"
#include "Host.h"
#include "Ini.h"
#include "Log.h"
#include "Mem.h"
#include "Puppet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

#include <windows.h>

namespace phantom::er::GraceRest
{
	namespace
	{
		// CSFadeImp (live 2026-11-04, 2.7.2.0; RTTI-checked at run time, so any build): +0x10.. nine CSFD4FadePlate pointers.
		// CSFD4FadePlate: +0x20 colour from (RGBA f32), +0x30 colour to, +0x48 time left (s), +0x58 length (s), +0x68 the name's
		// allocator, +0x70 the name (DLWString: inline UTF-16 while capacity <= 7, else a pointer), +0x80 its length, +0x88 capacity.
		constexpr std::uint32_t kFadePlates = 0x10;
		constexpr int           kFadePlateMax = 16;
		constexpr std::uint32_t kPlateFrom = 0x20;
		constexpr std::uint32_t kPlateTo = 0x30;
		constexpr std::uint32_t kPlateLeft = 0x48;
		constexpr std::uint32_t kPlateLength = 0x58;
		constexpr std::uint32_t kPlateName = 0x70;
		constexpr std::uint32_t kPlateNameLen = 0x80;
		constexpr std::uint32_t kPlateNameCap = 0x88;
		constexpr int           kSeatRaysPerFrame = 36;  // quiet.md §3.9: <= 36 rays a frame after the scan's frame

		// the fade
		std::uint8_t* g_plate{ nullptr };
		std::uint64_t g_plateVtable{ 0 };
		ULONGLONG     g_plateLookAt{ 0 };
		std::string   g_fadeWhy{ "not looked for yet" };
		bool          g_fadeLogged{ false };
		bool          g_lumaLogged{ false };
		grace::Plate  g_plateNow;
		float         g_alpha{ NAN };
		bool          g_dark{ false };
		std::uint64_t g_darkFrames{ 0 };

		// U12's record
		grace::BlackWatch g_watch;
		std::int64_t      g_hudMs{ -1 };
		bool              g_hudWas{ false };
		std::uint64_t     g_rests{ 0 };
		std::vector<std::int64_t> g_starts, g_ends, g_huds;
		bool              g_atGraceDark{ false };
		ULONGLONG         g_atGraceDarkAt{ 0 };
		std::uint64_t     g_chainBlacks{ 0 };

		// the seat
		grace::Gate   g_gate;
		grace::Search g_search;
		bool          g_haveCentre{ false };
		Vec3          g_centre;
		bool          g_centreFromAsset{ false };
		std::optional<grace::Spot> g_seat;
		bool          g_seatShown{ false };  // this seat was drawn (the echo came inside the black): she gets up from it
		std::uint32_t g_seq{ 0 };
		bool          g_dialog{ false };
		bool          g_dialogNew{ false };
		std::uint64_t g_seated{ 0 }, g_fadedIn{ 0 }, g_late{ 0 }, g_noBlack{ 0 }, g_tooLate{ 0 }, g_noSpot{ 0 }, g_light{ 0 }, g_cancelled{ 0 }, g_published{ 0 },
			g_notArmed{ 0 };
		std::string   g_lastWhyNot{ "no rest yet" };
		double        g_centreUs{ 0 }, g_searchUs{ 0 };
		int           g_searchRays{ 0 };

		// the recorder (`grace record on`): every ray the next searches cast, for tests/fixtures/quiet/
		bool        g_record{ false };
		std::string g_recordJson;
		int         g_recordCount{ 0 };

		grace::Rules Rules()
		{
			auto r = grace::MakeRules(Cfg().grace);
			r.occluderClear = std::max(0.0f, Cfg().render.occluderRadius) + 0.3f;  // the renderer's occluder fade (er/RenderShared.cpp)
			return r;
		}

		std::wstring NameOf(const std::uint8_t* a_plate)
		{
			const auto len = SafeRead<std::uint64_t>(a_plate + kPlateNameLen).value_or(0);
			const auto cap = SafeRead<std::uint64_t>(a_plate + kPlateNameCap).value_or(0);
			if (len == 0 || len > 64 || cap < len) {
				return {};
			}
			const std::uint8_t* chars = cap > 7 ? SafePtr(a_plate + kPlateName) : a_plate + kPlateName;
			std::wstring out(len, L'\0');
			if (!chars || !SafeReadBytes(chars, out.data(), len * 2)) {
				return {};
			}
			return out;
		}

		// The InGame plate, found by RTTI and name (never an index); its vtable cached to re-check every read.
		std::uint8_t* Plate()
		{
			if (g_plate && SafeRead<std::uint64_t>(g_plate).value_or(0) == g_plateVtable) {
				return g_plate;
			}
			g_plate = nullptr;
			const auto now = ::GetTickCount64();
			if (now - g_plateLookAt < 1000) {
				return nullptr;
			}
			g_plateLookAt = now;
			auto* fade = Addr().Instance(A::CSFade);
			if (!fade) {
				g_fadeWhy = "no CSFade instance";
				return nullptr;
			}
			if (const auto rtti = game::RttiName(fade); rtti != "CS::CSFadeImp") {
				g_fadeWhy = std::format("CSFade is {} (not the verified CS::CSFadeImp)", rtti.empty() ? "unknown" : rtti);
				return nullptr;
			}
			std::string seen;
			for (int i = 0; i < kFadePlateMax; ++i) {
				auto* p = SafePtr(fade + kFadePlates + 8 * i);
				if (!p || game::RttiName(p) != "CS::CSFD4FadePlate") {
					break;
				}
				const auto name = NameOf(p);
				std::string narrow;
				for (const wchar_t c : name) {
					narrow.push_back(c < 0x80 ? static_cast<char>(c) : '?');
				}
				seen += (seen.empty() ? "" : ", ") + narrow;
				if (name == L"InGame") {
					g_plate = p;
					g_plateVtable = SafeRead<std::uint64_t>(p).value_or(0);
					if (!g_fadeLogged) {
						g_fadeLogged = true;
						Log::Info("grace: Elden Ring's fade plate \"InGame\" found (CSFadeImp+{:#x}; plates: {}) -- the rest's black is observed from it",
							kFadePlates + 8 * i, seen);
					}
					g_fadeWhy = "ok";
					return g_plate;
				}
			}
			g_fadeWhy = std::format("no CSFD4FadePlate named InGame (plates: {})", seen.empty() ? "none" : seen);
			return nullptr;
		}

		bool ReadPlate(grace::Plate& a_out)
		{
			auto* p = Plate();
			return p && SafeReadBytes(p + kPlateFrom, a_out.from, 16) && SafeReadBytes(p + kPlateTo, a_out.to, 16) &&
			       SafeReadBytes(p + kPlateLeft, &a_out.left, 4) && SafeReadBytes(p + kPlateLength, &a_out.length, 4);
		}

		bool ReadCamera(grace::View& a_v)
		{
			const auto* cam = game::PersCam1();
			if (!cam) {
				return false;
			}
			const auto row = [&](int a_row) {
				const auto* f = reinterpret_cast<const float*>(cam + game::kCamMatrix + 16 * a_row);
				return Vec3{ f[0], f[1], f[2] };
			};
			a_v.right = Normalize(row(0));
			a_v.up = Normalize(row(1));
			a_v.fwd = Normalize(row(2) * Calibration::CamForwardRow());
			a_v.pos = row(3);
			a_v.fovY = *reinterpret_cast<const float*>(cam + game::kCamFov);
			a_v.aspect = 16.0f / 9.0f;
			a_v.valid = Finite(a_v.right) && Finite(a_v.up) && Finite(a_v.fwd) && Finite(a_v.pos) && a_v.fovY > 0.01f && a_v.fovY < 3.0f;
			return a_v.valid;
		}

		std::string J(const Vec3& a_v) { return std::format("[{:.3f}, {:.3f}, {:.3f}]", a_v.x, a_v.y, a_v.z); }

		// The live ray oracle: Snake's ground filter (what he stands on: Stormveil's wooden walkways included) or the ray filter.
		grace::RayFn LiveRays(const std::uint8_t* a_ignore)
		{
			return [a_ignore](const Vec3& a_from, const Vec3& a_d, grace::Ray a_kind) -> std::optional<grace::Hit> {
				Vec3       hit;
				const bool got = a_kind == grace::Ray::kGround ? Puppet::CastWorld(a_from, a_d, hit, a_ignore, true)
				                                               : game::CastRay(a_from, a_d, hit, a_ignore, Cfg().rayFilter);
				if (g_record) {
					g_recordJson += std::format("{}{{\"f\": {}, \"d\": {}, \"k\": {}, \"h\": {}}}", g_recordJson.ends_with("[") ? "\n  " : ",\n  ", J(a_from),
						J(a_d), a_kind == grace::Ray::kGround ? 0 : 1, got ? J(hit) : std::string("null"));
				}
				if (!got) {
					return std::nullopt;
				}
				return grace::Hit{ hit };
			};
		}

		std::optional<Vec3> AssetCentre(const Vec3& a_player, double& a_us)
		{
			LARGE_INTEGER t0, t1, fq;
			::QueryPerformanceCounter(&t0);
			std::vector<Vec3> graces;
			for (const auto& a : GeomProbe::Near(a_player, grace::kCentreMax + 1.0f, false)) {
				if (a.posValid && a.name.find("AEG099_060") != std::string::npos) {
					graces.push_back(a.pos);
				}
			}
			::QueryPerformanceCounter(&t1);
			::QueryPerformanceFrequency(&fq);
			a_us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
			return grace::NearestCentre(graces, a_player);
		}

		// The scene of a rest: the grace's centre and floor, Snake, the frozen view, the characters near it.
		grace::Scene MakeScene(const Frame& a_f, const Vec3& a_centre)
		{
			grace::Scene s;
			s.centre = a_centre;
			Vec3 hit;
			s.floorY = a_centre.y;
			if (Puppet::CastWorld(a_centre + Vec3{ 0, 1.5f, 0 }, Vec3{ 0, -3.0f, 0 }, hit, a_f.player.chr, true) && std::fabs(hit.y - a_centre.y) < 0.6f) {
				s.floorY = hit.y;
			}
			s.snake = Puppet::Active() ? Puppet::CurrentAnchor().pe : game::Position(a_f.player);
			ReadCamera(s.view);
			for (const auto& c : game::Characters(40)) {
				// (PlayerIns NPCs included: an NPC phantom such as White Mask Varre keeps the seat 2 m off as anyone does -- review)
				if (c.chr == a_f.player.chr || !Finite(c.pos) || LengthXZ(c.pos - a_centre) > 8.0f) {
					continue;
				}
				// The grace's own hidden team-0 character, 0.5 m under its light (every open-world grace checked has one).
				if (LengthXZ(c.pos - a_centre) < 1.0f && c.pos.y < a_centre.y + 0.5f) {
					continue;
				}
				s.others.push_back(c.pos);
			}
			return s;
		}

		void RecordBegin(const grace::Scene& a_s, std::string_view a_label)
		{
			g_recordJson = std::format("{{\n \"label\": \"{}\",\n \"centre\": {},\n \"floorY\": {:.3f},\n \"snake\": {},\n \"view\": {{\"valid\": {}, \"pos\": {}, \"fwd\": {}, "
			                           "\"right\": {}, \"up\": {}, \"fovY\": {:.4f}, \"aspect\": {:.4f}}},\n \"others\": [",
				a_label, J(a_s.centre), a_s.floorY, J(a_s.snake), a_s.view.valid ? "true" : "false", J(a_s.view.pos), J(a_s.view.fwd), J(a_s.view.right),
				J(a_s.view.up), a_s.view.fovY, a_s.view.aspect);
			for (std::size_t i = 0; i < a_s.others.size(); ++i) {
				g_recordJson += (i ? ", " : "") + J(a_s.others[i]);
			}
			g_recordJson += "],\n \"rays\": [";
		}

		void RecordEnd(const grace::Search& a_search)
		{
			if (g_recordJson.empty()) {
				return;
			}
			const auto& r = a_search.Result();
			g_recordJson += std::format("\n ],\n \"result\": {},\n \"evaluated\": {},\n \"passed\": {},\n \"rays_cast\": {}\n}}\n",
				r ? std::format("{{\"r\": {:.2f}, \"deg\": {:.1f}, \"feet\": {}, \"score\": {:.3f}, \"screenX\": {:.3f}, \"fallback\": {}}}", r->r, r->deg, J(r->feet),
						r->score, r->screenX, r->fallback ? "true" : "false")
				  : std::string("null"),
				a_search.Evaluated(), a_search.Passed(), a_search.Rays());
			const auto path = ModuleDir() + std::format(L"PhantomRingGraceRays-{}.json", ++g_recordCount);
			if (auto* f = _wfopen(path.c_str(), L"wb")) {
				std::fwrite(g_recordJson.data(), 1, g_recordJson.size(), f);
				std::fclose(f);
				Log::Info("grace: the search's rays written to PhantomRingGraceRays-{}.json ({} bytes)", g_recordCount, g_recordJson.size());
			}
			g_recordJson.clear();
		}

		const char* ArmRefusal(const Frame& a_f)
		{
			const auto& g = a_f.guest;
			if (!Cfg().grace.buddyRest) {
				return "[Grace] buddy_rest is off";
			}
			if (Cfg().grace.restDark != "fade") {
				return "[Grace] rest_dark isn't fade (no observed black: no grace rest)";
			}
			if (!g_plate) {
				return "Elden Ring's InGame fade plate isn't readable";
			}
			if (!Cfg().quiet.enabled || !Cfg().quiet.posts) {
				return "[Quiet] enabled / posts are off (the rest ships with posts)";
			}
			if (Host::Tarnished() && !Cfg().grace.tarnished) {
				return "Tarnished mode";
			}
			if (!a_f.guestReady || g.buddyType != Buddy::kQuiet || !(g.buddyFlags & proto::kBuddyPresent) || Buddy::Absent()) {
				return "Quiet isn't out";
			}
			if (!(g.buddyFlags & proto::kBuddyCanRest)) {
				return "the guest doesn't seat her (no kBuddyCanRest)";
			}
			return nullptr;
		}

		std::string Stats(std::vector<std::int64_t> a_v)
		{
			if (a_v.empty()) {
				return "-";
			}
			std::ranges::sort(a_v);
			return std::format("{} / {} / {} ms", a_v.front(), a_v[a_v.size() / 2], a_v.back());
		}

		void ApplyEvent(grace::Gate::Event a_e, const Frame& a_f)
		{
			using E = grace::Gate::Event;
			const auto now = ::GetTickCount64();
			const auto since = [&](std::uint64_t a_t) { return a_t ? static_cast<long long>(now - a_t) : -1LL; };
			switch (a_e) {
			case E::kBlack: {
				// The first black frame: the grace's centre and the search -- never before (the scan costs about a frame).
				const Vec3 player = game::Position(a_f.player);
				const auto asset = AssetCentre(player, g_centreUs);
				g_centreFromAsset = asset.has_value();
				g_centre = asset ? *asset : grace::FacingCentre(player, ErForward(game::Orientation(a_f.player), Puppet::CurrentAnchor().axes));
				g_haveCentre = true;
				auto scene = MakeScene(a_f, g_centre);
				if (g_record) {
					RecordBegin(scene, std::format("rest {}", g_rests));
				}
				g_search.Start(std::move(scene), Rules());
				g_searchUs = 0;
				g_searchRays = 0;
				Log::Info("grace: the rest's black {} ms after the press -> the grace at ({:.2f}, {:.2f}, {:.2f}) {} ({:.0f} us), the seat search starts",
					since(g_gate.PressMs()), g_centre.x, g_centre.y, g_centre.z, g_centreFromAsset ? "from its AEG099_060 asset" : "1.25 m along his facing (no asset)",
					g_centreUs);
				break;
			}
			case E::kNoBlack:
				++g_noBlack;
				Log::Info("grace: no black within {} ms of the press (a talk?) -- no seat this rest, nothing moves", Cfg().grace.seatBlackWaitMs);
				break;
			case E::kTooLate:
				++g_tooLate;
				g_search.Clear();
				Log::Info("grace: the seat search ran past the deadline ({} ms after the black) -- none this rest", Cfg().grace.seatDeadlineMs);
				break;
			case E::kLight:
				++g_light;
				g_search.Clear();
				Log::Info("grace: the black ended before the seat was ready -- none this rest");
				break;
			case E::kShown:
				++g_seated;
				g_seatShown = true;
				if (g_gate.ShownLate()) {
					++g_fadedIn;
				}
				Log::Info("grace: Quiet sits at the seat (the guest's echo {} ms after the publish, {}) -- drawn there", since(g_gate.PublishMs()),
					g_gate.ShownLate() ? "the screen back: faded in" : "still black");
				break;
			case E::kLateEcho:
				++g_late;
				Log::Info("grace: no echo from the guest within {} ms of the publish -- Quiet stays hidden through this rest (never a pop-in)", Cfg().grace.seatLateEchoMs);
				break;
			case E::kCancelled:
				++g_cancelled;
				g_search.Clear();
				g_seat.reset();
				g_seatShown = false;
				Log::Info("grace: an Elden Ring dialog -- the seat is cancelled; Quiet hidden while it lasts, then her post rule");
				break;
			case E::kEnded:
				// The seat is kept (not published: the gate is idle) until the next rest: er/Quiet.cpp makes it her post as she gets up,
				// drawn where she sat, so she never vanishes in view (live 2026-11-04, Rampart Tower: she did, the frame after he left).
				g_search.Clear();
				g_haveCentre = false;
				Log::Info("grace: Snake left the grace (kHostAtGrace falls) -- {}", g_seatShown ? "Quiet gets up: the seat is her post until her post rule moves her"
				                                                                              : "she wasn't seated this rest: nothing to undo");
				break;
			default:
				break;
			}
		}
	}

	void OnRest(const Frame& a_f)
	{
		++g_rests;
		g_watch.Press(::GetTickCount64());
		g_hudMs = -1;
		g_hudWas = a_f.hudHidden;
		if (const char* why = ArmRefusal(a_f)) {
			++g_notArmed;
			g_lastWhyNot = why;
			if (g_gate.Current() != grace::Gate::Phase::kShown) {
				g_gate.Clear();
			}
			Log::Info("grace: rest #{} -- no seat ({})", g_rests, why);
			return;
		}
		const bool reRest = g_gate.Current() == grace::Gate::Phase::kShown;
		g_gate.Rest(::GetTickCount64());
		g_lastWhyNot = "armed";
		if (reRest) {
			Log::Info("grace: rest #{} at the same grace -- Quiet stays where she sits", g_rests);
			return;
		}
		g_seat.reset();
		g_seatShown = false;
		g_haveCentre = false;
		g_search.Clear();
		Log::Info("grace: rest #{} -- armed: the seat waits for the observed black (within {} ms)", g_rests, Cfg().grace.seatBlackWaitMs);
	}

	void OnDialog()
	{
		g_dialog = true;
		g_dialogNew = true;
	}

	void Tick(const Frame& a_f)
	{
		const auto now = ::GetTickCount64();
		const auto& gc = Cfg().grace;
		if (gc.restDark == "luma" && !g_lumaLogged) {
			g_lumaLogged = true;
			Log::Warn("grace: [Grace] rest_dark = luma isn't built (U12 found Elden Ring's InGame fade plate: rest_dark = fade) -- no grace rest");
		}
		// The fade: every frame while in the world (it is cheap: four reads).
		g_alpha = NAN;
		g_dark = false;
		if (a_f.inGame && ReadPlate(g_plateNow)) {
			g_alpha = grace::FadeAlpha(g_plateNow);
			g_dark = grace::PlateDark(g_plateNow);
			if (!std::isfinite(g_alpha)) {
				g_plate = nullptr;
				g_fadeWhy = "the InGame plate's numbers aren't sane (not the verified layout)";
			}
		}
		g_darkFrames += g_dark ? 1 : 0;
		// U12: the rest's black after the press, the HUD's hide.
		if (g_watch.Active()) {
			if (a_f.hudHidden && !g_hudWas && g_hudMs < 0) {
				g_hudMs = static_cast<std::int64_t>(now - g_watch.PressMs());
			}
			g_hudWas = a_f.hudHidden;
			if (g_watch.Update(now, g_dark)) {
				if (g_watch.StartMs() >= 0) {
					g_starts.push_back(g_watch.StartMs());
					g_ends.push_back(g_watch.EndMs());
					if (g_hudMs >= 0) {
						g_huds.push_back(g_hudMs);
					}
					Log::Info("grace: U12 rest #{} -- black from +{} to +{} ms after the press ({} ms; the InGame fade plate), HUD hid at {}", g_rests,
						g_watch.StartMs(), g_watch.EndMs(), g_watch.EndMs() - g_watch.StartMs(), g_hudMs >= 0 ? std::format("+{} ms", g_hudMs) : std::string("-"));
				} else {
					Log::Info("grace: U12 rest #{} -- no black within {} ms of the press ({})", g_rests, grace::BlackWatch::kWindowMs,
						g_plate ? "the plate read" : g_fadeWhy);
				}
			}
		}
		// Another black while he sits there (Pass time): measured too.
		const bool atGrace = Host::AtGrace();
		if (atGrace && g_dark && !g_atGraceDark && !g_watch.Active()) {
			g_atGraceDarkAt = now;
		}
		if (!g_dark && g_atGraceDark && g_atGraceDarkAt) {
			++g_chainBlacks;
			Log::Info("grace: another black at the grace ({} ms: Pass time?) -- Quiet {}", now - g_atGraceDarkAt,
				g_gate.AtSeat() ? "stays seated" : "is where she was");
			g_atGraceDarkAt = 0;
		}
		g_atGraceDark = g_dark && atGrace;
		if (!a_f.interacting) {
			g_dialog = false;
		}
		// The gate.
		if (g_gate.Current() != grace::Gate::Phase::kIdle) {
			const auto& g = a_f.guest;
			grace::Gate::In in;
			in.nowMs = now;
			in.dark = g_dark;
			in.atGrace = atGrace && a_f.inGame && !a_f.dead && !a_f.dying;
			in.echo = g_seq && a_f.guestReady && (g.buddyFlags & proto::kBuddySeated) && g.graceSeqSeated == g_seq;
			in.dialog = g_dialog || g_dialogNew;
			g_dialogNew = false;
			const auto rules = Rules();
			ApplyEvent(g_gate.Update(in, rules), a_f);
			// The seat search: a ray budget a frame, inside the black.
			if (g_gate.Searching() && g_search.Running()) {
				LARGE_INTEGER t0, t1, fq;
				::QueryPerformanceCounter(&t0);
				const int before = g_search.Rays();
				g_search.Step(kSeatRaysPerFrame, LiveRays(a_f.player.chr));
				::QueryPerformanceCounter(&t1);
				::QueryPerformanceFrequency(&fq);
				g_searchUs += static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
				g_searchRays += g_search.Rays() - before;
				if (g_search.Done()) {
					if (g_record) {
						RecordEnd(g_search);
					}
					const auto& r = g_search.Result();
					const auto  e = g_gate.SearchDone(now, r.has_value(), g_dark, rules);
					Log::Info("grace: seat search done {} ms after the black: {} -- {} ({} rays in {:.0f} us, {:.1f} us a ray){}", now - g_gate.BlackMs(),
						r ? std::format("{} m @{:.0f} deg{}, {:.2f} m from Snake, screen x {:.2f}, score {:.2f}", r->r, r->deg, r->fallback ? " (the fallback)" : "",
								LengthXZ(r->feet - g_search.CurrentScene().snake), r->screenX, r->score)
						  : std::string("no seat"),
						g_search.Summary(), g_searchRays, g_searchUs, g_searchRays ? g_searchUs / g_searchRays : 0.0,
						e == grace::Gate::Event::kPublish ? "" : std::format(" -> {}", grace::Name(e)));
					if (e == grace::Gate::Event::kPublish) {
						g_seat = r;
						++g_seq;
						++g_published;
						Log::Info("grace: the seat published (graceSeq {}) {} ms after the black{} -- Quiet hidden until the guest's echo", g_seq,
							now - g_gate.BlackMs(), g_gate.ReadyForReload(rules) ? " (in time for a reload placement too)" : "");
					} else if (e == grace::Gate::Event::kNoSpot) {
						++g_noSpot;
					} else {
						ApplyEvent(e, a_f);
					}
				}
			}
		}
	}

	void Fill(proto::HostState& a_s, const Anchor& a_a, bool a_atGrace)
	{
		for (int i = 0; i < 3; ++i) {
			a_s.graceCentre[i] = a_s.graceSeat[i] = NAN;
		}
		a_s.graceSeatYaw = 0.0f;
		a_s.graceSeq = g_seq;
		if (a_atGrace) {
			a_s.flags |= proto::kHostAtGrace;
		}
		if (!a_atGrace || g_gate.Current() == grace::Gate::Phase::kIdle) {
			return;
		}
		if (g_haveCentre) {
			const Vec3 c = a_a.PointToMgsv(g_centre);
			a_s.graceCentre[0] = c.x;
			a_s.graceCentre[1] = c.y;
			a_s.graceCentre[2] = c.z;
		}
		if (g_seat && g_gate.SeatPublished()) {
			const Vec3 p = a_a.PointToMgsv(g_seat->feet);
			a_s.graceSeat[0] = p.x;
			a_s.graceSeat[1] = p.y;
			a_s.graceSeat[2] = p.z;
			const Vec3 dir = a_a.DirToMgsv(Vec3{ std::sin(g_seat->heading), 0, std::cos(g_seat->heading) });
			a_s.graceSeatYaw = a_a.axes.mgsvYawSign * Heading(dir);
		}
	}

	bool  AtSeat() { return g_gate.AtSeat() && g_seat.has_value(); }
	float SeatAlpha() { return g_gate.SeatAlpha(::GetTickCount64(), Rules()); }
	bool  LastSeat(Vec3& a_feet, float& a_heading)
	{
		if (!g_seat || !g_seatShown) {
			return false;
		}
		a_feet = g_seat->feet;
		a_heading = g_seat->heading;
		return true;
	}
	bool  Hide() { return g_gate.Hide(); }
	Vec3  Seat() { return g_seat ? g_seat->feet : Vec3{ NAN, NAN, NAN }; }
	float SeatHeading() { return g_seat ? g_seat->heading : 0.0f; }
	bool  Dark() { return g_dark; }
	bool  FadeUsable() { return g_plate != nullptr; }

	void OnWorldShift(const Vec3& a_d)
	{
		g_centre += a_d;
		if (g_seat) {
			g_seat->feet += a_d;
		}
		g_search.Shift(a_d);
	}

	std::string Describe()
	{
		const auto& gc = Cfg().grace;
		return std::format(
			"grace rest ([Grace] buddy_rest {}, rest_dark {}): the InGame fade plate {} (alpha {:.3f}{}; {} dark frames); gate {}, graceSeq {}; "
			"rests {} (not armed {}: last \"{}\"), seated {} ({} faded in after the black), no echo {}, no black {}, too late {}, the black ended first {}, no seat found {}, "
			"cancelled {}, published {}; U12 over {} rests: black start {} / end {} after the press (min / median / max), HUD hid {}; blacks at the grace "
			"since (Pass time) {}{}{}",
			gc.buddyRest ? "on" : "off", gc.restDark, g_plate ? "readable" : g_fadeWhy, g_alpha, g_dark ? ", BLACK" : "", g_darkFrames, grace::Name(g_gate.Current()),
			g_seq, g_rests, g_notArmed, g_lastWhyNot, g_seated, g_fadedIn, g_late, g_noBlack, g_tooLate, g_light, g_noSpot, g_cancelled, g_published, g_starts.size(),
			Stats(g_starts), Stats(g_ends), Stats(g_huds), g_chainBlacks,
			g_seat ? std::format("; seat ({:.2f}, {:.2f}, {:.2f}) {:.1f} m @{:.0f} deg{}", g_seat->feet.x, g_seat->feet.y, g_seat->feet.z, g_seat->r, g_seat->deg,
						 g_seat->fallback ? " (fallback)" : "")
				   : std::string(),
			g_search.Running() || g_search.Done() ? std::format("; search: {}", g_search.Summary()) : std::string());
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		auto& gc = Cfg().grace;
		if (a_w.size() < 2 || a_w[1] == "status") {
			return Describe();
		}
		if (a_w[1] == "fade") {
			grace::Plate p;
			if (!ReadPlate(p)) {
				return std::format("fade: {}", g_fadeWhy);
			}
			float      rgb[3]{};
			const float a = grace::FadeAlpha(p, rgb);
			return std::format("InGame fade plate {}: from ({:.2f} {:.2f} {:.2f} {:.2f}) to ({:.2f} {:.2f} {:.2f} {:.2f}) left {:.3f} / {:.3f} s -> colour ({:.2f} {:.2f} "
			                   "{:.2f}) alpha {:.3f}{}",
				static_cast<void*>(g_plate), p.from[0], p.from[1], p.from[2], p.from[3], p.to[0], p.to[1], p.to[2], p.to[3], p.left, p.length, rgb[0], rgb[1], rgb[2], a,
				grace::PlateDark(p) ? " BLACK" : "");
		}
		if (a_w[1] == "on" || a_w[1] == "off") {
			// The whole rest for a test run (not saved): [Grace] buddy_rest and rest_dark = fade.
			gc.buddyRest = a_w[1] == "on";
			if (gc.buddyRest) {
				gc.restDark = "fade";
			}
			return Describe();
		}
		if (a_w[1] == "record" && a_w.size() > 2) {
			g_record = a_w[2] == "on";
			return std::format("grace: {} (PhantomRingGraceRays-N.json next to the DLL, one per search)", g_record ? "every search's rays recorded" : "no recording");
		}
		if (a_w[1] == "survey") {
			// The seat search right now round the nearest grace (no rest: the view is the camera's now), every ray written out.
			if (!a_f.player || !a_f.inGame) {
				return "grace survey: not in the world";
			}
			const Vec3 player = game::Position(a_f.player);
			double     us = 0;
			const auto centre = AssetCentre(player, us);
			if (!centre) {
				std::vector<Vec3> any;
				for (const auto& x : GeomProbe::Near(player, 30.0f, false)) {
					if (x.posValid && x.name.find("AEG099_060") != std::string::npos) {
						any.push_back(x.pos);
					}
				}
				return std::format("grace survey: no AEG099_060 asset within {:.1f} m ({} within 30 m)", grace::kCentreMax, any.size());
			}
			// `grace survey all [label]`: every candidate looked at (no early stop): a full fixture for the offline tests.
			const bool all = a_w.size() > 2 && a_w[2] == "all";
			const auto label = a_w.size() > (all ? 3u : 2u) ? a_w[all ? 3 : 2] : std::string("survey");
			const bool was = g_record;
			g_record = true;
			auto scene = MakeScene(a_f, *centre);
			RecordBegin(scene, all ? label + " (all)" : label);
			auto rules = Rules();
			if (all) {
				rules.goodEnough = -1e9f;
			}
			grace::Search s;
			s.Start(std::move(scene), rules);
			const auto ray = LiveRays(a_f.player.chr);
			while (!s.Step(100000, ray)) {
			}
			if (all) {
				g_recordJson += "\n ],\n \"all\": true,\n \"rays_tail\": [";  // (RecordEnd closes the array it thinks is open)
			}
			RecordEnd(s);
			g_record = was;
			const auto& r = s.Result();
			return std::format("grace survey at ({:.2f}, {:.2f}, {:.2f}) (asset scan {:.0f} us): {} -- {}", centre->x, centre->y, centre->z, us,
				r ? std::format("seat {} m @{:.0f} deg ({:.2f}, {:.2f}, {:.2f}){}, {:.2f} m from Snake, screen x {:.2f}, score {:.2f}", r->r, r->deg, r->feet.x, r->feet.y,
						r->feet.z, r->fallback ? " (the fallback)" : "", LengthXZ(r->feet - s.CurrentScene().snake), r->screenX, r->score)
				  : std::string("no seat"),
				s.Summary());
		}
		if (a_w[1] == "set" && a_w.size() > 3) {
			const auto& k = a_w[2];
			const auto& v = a_w[3];
			const float f = std::strtof(v.c_str(), nullptr);
			const int   n = static_cast<int>(std::strtol(v.c_str(), nullptr, 10));
			if (k == "buddy_rest") gc.buddyRest = v == "on" || v == "true" || v == "1";
			else if (k == "rest_dark") gc.restDark = v == "fade" || v == "luma" ? v : "none";
			else if (k == "tarnished") gc.tarnished = v == "on" || v == "true" || v == "1";
			else if (k == "seat_black_wait_ms") gc.seatBlackWaitMs = std::max(0, n);
			else if (k == "seat_deadline_ms") gc.seatDeadlineMs = std::max(0, n);
			else if (k == "seat_screen_x") gc.seatScreenX = f;
			else if (k == "seat_menu_x") gc.seatMenuX = f;
			else if (k == "seat_char_clear") gc.seatCharClear = std::max(0.0f, f);
			else if (k == "seat_snake_clear") gc.seatSnakeClear = std::max(0.0f, f);
			else return std::format("grace set: unknown key {}", k);
			return Describe();
		}
		return "grace [status | fade | on | off | record on|off | survey [all] [label] | set <key> <value>]";
	}
}
