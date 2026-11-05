#include "Quiet.h"

#include "Anchor.h"
#include "Buddy.h"
#include "Calibration.h"
#include "Combat.h"
#include "Config.h"
#include "Cqc.h"
#include "CqcLogic.h"
#include "DogLogic.h"
#include "Fulton.h"
#include "Game.h"
#include "GraceRest.h"
#include "Host.h"
#include "Hud.h"
#include "Ini.h"
#include "InputBridge.h"
#include "Log.h"
#include "Marking.h"
#include "MarkingLogic.h"
#include "Mem.h"
#include "Puppet.h"
#include "QuietLogic.h"
#include "RenderMarkers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <optional>

#include <windows.h>

namespace phantom::er::Quiet
{
	namespace
	{
		constexpr std::uint16_t kLb = 0x0100;       // XINPUT_GAMEPAD_LEFT_SHOULDER: MGSV's call menu while held
		constexpr std::uint8_t  kRenderBit = 0x08;  // ChrIns+0x1C5 bit3 enable_render
		constexpr ULONGLONG     kCharsMs = 250;     // the characters around, read 4 times a second
		constexpr ULONGLONG     kPickMs = 250;      // the pick 4 times a second while LB / the binoculars (or just after)...
		constexpr ULONGLONG     kPickIdleMs = 1000; // ...once a second otherwise
		constexpr ULONGLONG     kLookAfterMs = 2000;
		constexpr ULONGLONG     kHintMs = 600;      // LB held this long: the mark hint
		constexpr ULONGLONG     kSnakeCheckMs = 500;
		constexpr ULONGLONG     kLostSnakeMs = 2000;
		constexpr ULONGLONG     kSearchCooldownMs = 1500;
		constexpr ULONGLONG     kAfterGraceMs = 1500;  // a post search no sooner than this after he left the grace
		constexpr ULONGLONG     kHitDedupeMs = 1000;
		constexpr std::uint32_t kPhysHitHeight = 0x2F0;
		constexpr std::uint32_t kPhysHitRadius = 0x2F4;
		constexpr std::uint32_t kPhysChrHitHeight = 0x2E0;
		constexpr std::uint32_t kPhysChrHitRadius = 0x2E4;
		constexpr float         kWhite[4]{ 1.0f, 1.0f, 1.0f, 0.95f };
		constexpr float         kQuietColour[4]{ 0.45f, 0.95f, 0.85f, 1.0f };  // MGSV's buddy teal: a mark MGSV holds on his stand-in
		constexpr float         kSnakeChest = 1.25f;

		struct Chr
		{
			game::Character                c;
			bool                           hostile{ false };
			bool                           rendered{ true };
			std::optional<combat::Capsule> body;
		};

		std::atomic<std::uint64_t> g_target{ 0 }, g_threat{ 0 };
		std::string                g_targetName;
		std::vector<Chr>           g_chrs;
		ULONGLONG                  g_chrsAt{ 0 };
		bool                       g_wasOut{ false };
		ULONGLONG                  g_outLostAt{ 0 };
		constexpr ULONGLONG        kOutLostMs = 1000;
		// her post
		quiet::MoveWatch           g_move;
		quiet::PostSearch          g_search;
		quiet::Fader               g_fader;
		quiet::Mode                g_mode{ quiet::Mode::kNone };
		quiet::Mode                g_postMode{ quiet::Mode::kNone };
		Vec3                       g_searchCentre;
		bool                       g_everPosted{ false };
		bool                       g_retryNear{ false };
		std::uint32_t              g_seq{ 0 };
		ULONGLONG                  g_searchAt{ 0 }, g_searchStartedAt{ 0 };
		ULONGLONG                  g_noPostAt{ 0 };    // the last search found no post at all (indoors, a cave): back off
		Vec3                       g_noPostFrom;
		constexpr ULONGLONG        kNoPostBackoffMs = 6000;
		constexpr float            kNoPostMoved = 8.0f;
		const char*                g_searchWhy{ "" };
		double                     g_searchUs{ 0 };
		Vec3                       g_lastPe;
		bool                       g_havePe{ false };
		float                      g_speed{ 0 };
		ULONGLONG                  g_snakeCheckAt{ 0 }, g_snakeSeenAt{ 0 };
		bool                       g_targetUnseen{ false };
		bool                       g_wasSeated{ false };
		ULONGLONG                  g_graceLeftAt{ 0 };
		bool                       g_atGraceWas{ false };
		// the pick
		dog::PickLatch             g_latch;
		quiet::PickResult          g_last;
		std::string                g_lastWhy{ "not run yet" };
		ULONGLONG                  g_pickAt{ 0 }, g_lookAt{ 0 };
		bool                       g_lbDown{ false };
		ULONGLONG                  g_lbDownAt{ 0 };
		bool                       g_hintSaid{ false };
		bool                       g_testBoss{ false };
		// spotting
		quiet::Spotter             g_spotter;
		quiet::SpotText            g_spotText;
		ULONGLONG                  g_spotAt{ 0 };
		std::size_t                g_spotNext{ 0 };
		// shots
		std::map<std::uint64_t, ULONGLONG> g_lastHit;
		std::array<std::uint64_t, 5>       g_phases{};
		std::array<std::uint64_t, static_cast<std::size_t>(quiet::ShotAct::kCount)> g_acts{};
		std::uint64_t g_posts{ 0 }, g_noPosts{ 0 }, g_searches{ 0 }, g_hidden{ 0 }, g_picks{ 0 }, g_pickChanges{ 0 }, g_rays{ 0 }, g_spotted{ 0 },
			g_texts{ 0 }, g_hits{ 0 }, g_hitsLanded{ 0 }, g_hints{ 0 }, g_echoLate{ 0 }, g_kept{ 0 };
		// the recorder (`quiet record on`, `quiet survey`): every ray of the next post searches, for tests/fixtures/quiet/
		bool        g_record{ false };
		std::string g_recordJson;
		int         g_recordCount{ 0 };

		quiet::Rules Rules() { return quiet::MakeRules(Cfg().quiet); }

		bool Out(const Frame& a_f)
		{
			const auto& g = a_f.guest;
			return a_f.guestReady && a_f.player && a_f.inGame && !a_f.loading && g.buddyType == Buddy::kQuiet && (g.buddyFlags & proto::kBuddyPresent) &&
			       !Buddy::Absent();
		}

		bool Live(const Frame& a_f)
		{
			return Cfg().quiet.enabled && Out(a_f) && Puppet::Active() && !a_f.dead && !a_f.dying && !a_f.cutscene && !Host::Tarnished();
		}

		// Never a friend: both of Elden Ring's team reads (er/Dog.cpp StrictHostile, the same rule).
		bool StrictHostile(const Frame& a_f, const game::Character& a_c, std::uint8_t a_myTeam)
		{
			return a_c.isEnemy && !a_c.isPlayer && a_c.chr != a_f.player.chr && a_c.team != a_myTeam && Combat::HostileToPlayer(a_f, a_c.team) &&
			       Combat::HostileChr(a_f, a_c.chr);
		}

		bool SaneCapsule(float a_h, float a_r) { return std::isfinite(a_h) && std::isfinite(a_r) && a_r >= 0.08f && a_r <= 10.0f && a_h >= 0.2f && a_h <= 40.0f; }

		std::optional<combat::Capsule> Body(const game::Character& a_c)
		{
			auto* modules = SafePtr(a_c.chr + game::kChrModules);
			auto* phys = modules ? SafePtr(modules + game::kModPhysics) : nullptr;
			if (!phys) {
				return std::nullopt;
			}
			for (const auto& [hOff, rOff] : { std::pair{ kPhysHitHeight, kPhysHitRadius }, std::pair{ kPhysChrHitHeight, kPhysChrHitRadius } }) {
				const float h = SafeRead<float>(phys + hOff).value_or(NAN);
				const float r = SafeRead<float>(phys + rOff).value_or(NAN);
				if (SaneCapsule(h, r)) {
					return combat::ChrCapsule(a_c.pos, h, r);
				}
			}
			return std::nullopt;
		}

		// The PlayerIns NPCs (an NPC phantom such as White Mask Varre, a summoned spirit's or a co-op player's character) as Around saw them
		// last: never candidates, but her post keeps its distance from them as from anyone (review).
		std::vector<Vec3> g_phantoms;

		std::vector<Chr> Around(const Frame& a_f, std::vector<Vec3>* a_phantoms = nullptr)
		{
			std::vector<Chr> out;
			const auto       myTeam = SafeRead<std::uint8_t>(a_f.player.chr + game::kChrTeam).value_or(0);
			if (a_phantoms) {
				a_phantoms->clear();
			}
			for (const auto& c : game::Characters(128)) {
				if (c.isPlayer && c.chr != a_f.player.chr && Finite(c.pos) && a_phantoms) {
					a_phantoms->push_back(c.pos);
				}
				if (c.isPlayer || c.chr == a_f.player.chr || !Finite(c.pos)) {
					continue;
				}
				Chr x{ c };
				x.hostile = StrictHostile(a_f, c, myTeam);
				x.rendered = (SafeRead<std::uint8_t>(c.chr + game::kChrFlags1C5).value_or(kRenderBit) & kRenderBit) != 0 && c.maxHp > 0;
				x.body = Body(c);
				out.push_back(std::move(x));
			}
			return out;
		}

		std::string NameOf(const game::Character& a_c) { return std::format("npc {} team {} hp {}/{} ({:#x})", a_c.npcParam, a_c.team, a_c.hp, a_c.maxHp, a_c.handle); }

		Vec3 ChestOf(const Chr& a_x) { return a_x.c.pos + Vec3{ 0, marking::AimHeight(a_x.body, a_x.c.pos, 0.7f), 0 }; }

		bool Boss(const Chr& a_x) { return g_testBoss || Cqc::IsBoss(a_x.c.npcParam, a_x.c.maxHp); }

		bool ReadCamera(marking::Camera& a_out)
		{
			const auto* cam = game::PersCam1();
			if (!cam) {
				return false;
			}
			const auto row = [&](int a_row) {
				const auto* f = reinterpret_cast<const float*>(cam + game::kCamMatrix + 16 * a_row);
				return Vec3{ f[0], f[1], f[2] };
			};
			a_out.right = Normalize(row(0));
			a_out.up = Normalize(row(1));
			a_out.fwd = Normalize(row(2) * Calibration::CamForwardRow());
			a_out.pos = row(3);
			a_out.fovY = *reinterpret_cast<const float*>(cam + game::kCamFov);
			return Finite(a_out.right) && Finite(a_out.up) && Finite(a_out.fwd) && Finite(a_out.pos) && a_out.fovY > 0.01f && a_out.fovY < 3.0f;
		}

		bool InView(const marking::Camera& a_c, const Vec3& a_p)
		{
			const Vec3  d = a_p - a_c.pos;
			const float z = Dot(d, a_c.fwd);
			if (z < 0.2f || z > 250.0f) {
				return false;
			}
			const float ty = std::tan(a_c.fovY * 0.5f) * 1.15f;
			const float tx = ty * 16.0f / 9.0f;
			return std::fabs(Dot(d, a_c.right)) <= tx * z + 1.0f && std::fabs(Dot(d, a_c.up)) <= ty * z + 1.0f;
		}

		std::string J(const Vec3& a_v) { return std::format("[{:.3f}, {:.3f}, {:.3f}]", a_v.x, a_v.y, a_v.z); }

		// The live ray oracle: Snake's ground filter or the ray filter, ignoring the Tarnished.
		quiet::RayFn LiveRays(const std::uint8_t* a_ignore)
		{
			return [a_ignore](const Vec3& a_from, const Vec3& a_d, quiet::Ray a_kind) -> std::optional<quiet::Hit> {
				Vec3       hit;
				const bool got = a_kind == quiet::Ray::kGround ? Puppet::CastWorld(a_from, a_d, hit, a_ignore, true)
				                                               : game::CastRay(a_from, a_d, hit, a_ignore, Cfg().rayFilter);
				++g_rays;
				if (g_record) {
					g_recordJson += std::format("{}{{\"f\": {}, \"d\": {}, \"k\": {}, \"h\": {}}}", g_recordJson.ends_with("[") ? "\n  " : ",\n  ", J(a_from),
						J(a_d), a_kind == quiet::Ray::kGround ? 0 : 1, got ? J(hit) : std::string("null"));
				}
				if (!got) {
					return std::nullopt;
				}
				return quiet::Hit{ hit };
			};
		}

		// The line from a_from to his chest: clear, or stopped on his own body (marking::RayClear).
		bool Sees(const Vec3& a_from, const Chr& a_x, const std::uint8_t* a_ignore)
		{
			const Vec3 to = ChestOf(a_x);
			Vec3       hit;
			++g_rays;
			if (!game::CastRay(a_from, to - a_from, hit, a_ignore, Cfg().rayFilter)) {
				return true;
			}
			return marking::RayClear(a_from, to, hit, a_x.body);
		}

		Vec3 SnakeFeet(const Frame& a_f) { return Puppet::Active() ? Puppet::CurrentAnchor().pe : game::Position(a_f.player); }

		std::optional<Vec3> SightPoint()
		{
			const auto& p = g_fader.Shown();
			if (!p || GraceRest::AtSeat() || GraceRest::Hide()) {
				return std::nullopt;
			}
			return p->feet + Vec3{ 0, Rules().sight, 0 };
		}

		quiet::PostScene Scene(const Frame& a_f, const marking::Camera& a_cam, const quiet::Rules& a_r)
		{
			quiet::PostScene s;
			s.snake = SnakeFeet(a_f);
			s.snakeChest = s.snake + Vec3{ 0, kSnakeChest, 0 };
			s.camPos = a_cam.pos;
			s.camFwd = a_cam.fwd;
			s.fovY = a_cam.fovY;
			Vec3 hit;
			s.aimPoint = game::CastRay(a_cam.pos, a_cam.fwd * 60.0f, hit, a_f.player.chr, Cfg().rayFilter) ? hit : a_cam.pos + a_cam.fwd * 60.0f;
			float farthest = 0;
			for (const float r : a_r.coverRings) {
				farthest = std::max(farthest, r);
			}
			for (const auto& x : g_chrs) {
				const float d = Length(x.c.pos - s.snake);
				if (x.hostile && !x.c.dead && x.c.hp > 0 && !Boss(x) && d <= a_r.hostileRange) {
					s.hostiles.push_back({ x.c.handle, x.c.pos, ChestOf(x) });
				}
				if (d <= farthest + a_r.charClear + 5.0f) {
					s.characters.push_back(x.c.pos);
				}
			}
			for (const auto& p : g_phantoms) {
				if (Length(p - s.snake) <= farthest + a_r.charClear + 5.0f) {
					s.characters.push_back(p);  // spacing only: never a hostile, a pick or a shot
				}
			}
			return s;
		}

		bool HostileNear(const Frame& a_f, const quiet::Rules& a_r)
		{
			const Vec3 snake = SnakeFeet(a_f);
			return std::ranges::any_of(g_chrs, [&](const Chr& x) {
				return x.hostile && !x.c.dead && x.c.hp > 0 && x.rendered && !Boss(x) && Length(x.c.pos - snake) <= a_r.hostileRange;
			});
		}

		void RecordBegin(const quiet::PostScene& a_s, std::string_view a_label)
		{
			g_recordJson = std::format("{{\n \"label\": \"{}\",\n \"snake\": {},\n \"snakeChest\": {},\n \"camPos\": {},\n \"camFwd\": {},\n \"fovY\": {:.4f},\n "
			                           "\"aimPoint\": {},\n \"hostiles\": [",
				a_label, J(a_s.snake), J(a_s.snakeChest), J(a_s.camPos), J(a_s.camFwd), a_s.fovY, J(a_s.aimPoint));
			for (std::size_t i = 0; i < a_s.hostiles.size(); ++i) {
				g_recordJson += std::format("{}{{\"handle\": \"{:#x}\", \"feet\": {}, \"chest\": {}}}", i ? ", " : "", a_s.hostiles[i].handle, J(a_s.hostiles[i].feet),
					J(a_s.hostiles[i].chest));
			}
			g_recordJson += "],\n \"characters\": [";
			for (std::size_t i = 0; i < a_s.characters.size(); ++i) {
				g_recordJson += (i ? ", " : "") + J(a_s.characters[i]);
			}
			g_recordJson += "],\n \"rays\": [";
		}

		void RecordEnd(std::string_view a_tail)
		{
			if (g_recordJson.empty()) {
				return;
			}
			g_recordJson += std::format("\n ],\n{}\n}}\n", a_tail);
			const auto path = ModuleDir() + std::format(L"PhantomRingQuietRays-{}.json", ++g_recordCount);
			if (auto* f = _wfopen(path.c_str(), L"wb")) {
				std::fwrite(g_recordJson.data(), 1, g_recordJson.size(), f);
				std::fclose(f);
				Log::Info("quiet: the post search's rays written to PhantomRingQuietRays-{}.json ({} bytes)", g_recordCount, g_recordJson.size());
			}
			g_recordJson.clear();
		}

		std::string PostLine(const quiet::Post& a_p, const Vec3& a_snake)
		{
			return std::format("{} {:.0f} m @{:.0f} deg ({:.2f}, {:.2f}, {:.2f}), {:+.1f} m over Snake, {:.1f} m from him, sees {} hostile(s), score {:.2f}", quiet::Name(a_p.mode),
				a_p.ring, a_p.deg, a_p.feet.x, a_p.feet.y, a_p.feet.z, a_p.height, Length(a_p.feet - a_snake), a_p.seen, a_p.score);
		}

		void StartSearch(const Frame& a_f, quiet::Mode a_mode, const char* a_why)
		{
			marking::Camera cam;
			if (!ReadCamera(cam)) {
				return;
			}
			const auto rules = Rules();
			auto       scene = Scene(a_f, cam, rules);
			if (g_record) {
				RecordBegin(scene, std::format("{} search {}", quiet::Name(a_mode), g_searches + 1));
			}
			g_searchCentre = scene.snake;
			g_search.Start(std::move(scene), a_mode, rules);
			g_searchAt = g_searchStartedAt = ::GetTickCount64();
			g_searchWhy = a_why;
			g_searchUs = 0;
			++g_searches;
		}

		void Publish(const char* a_what)
		{
			++g_seq;
			g_everPosted = true;
			const auto& p = g_fader.Shown();
			Log::Info("quiet: post {} published (buddyPostSeq {}){}", a_what, g_seq,
				p ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", p->feet.x, p->feet.y, p->feet.z) : std::string());
		}

		// The search finished: move her (with the fade in view), keep her, or none.
		void SearchDone(const Frame& a_f, const marking::Camera& a_cam, bool a_camOk)
		{
			const auto  rules = Rules();
			const auto& best = g_search.Best();
			const auto& cur = g_fader.Shown();
			const Vec3  snake = SnakeFeet(a_f);
			const bool  inView = a_camOk && cur && InView(a_cam, cur->feet + Vec3{ 0, 0.8f, 0 });
			Log::Info("quiet: post search ({}) done in {} ms: {} -- {} ({:.0f} us, {:.1f} us a ray)", g_searchWhy, ::GetTickCount64() - g_searchStartedAt,
				best ? PostLine(*best, snake) : std::string("none passed"), g_search.Summary(), g_searchUs, g_search.Rays() ? g_searchUs / g_search.Rays() : 0.0);
			if (g_record) {
				RecordEnd(std::format(" \"mode\": \"{}\",\n \"best\": {}", quiet::Name(g_search.CurrentMode()),
					best ? std::format("{{\"ring\": {:.0f}, \"deg\": {:.0f}, \"feet\": {}, \"seen\": {}, \"score\": {:.3f}}}", best->ring, best->deg, J(best->feet),
							   best->seen, best->score)
						 : std::string("null")));
			}
			// Her current post judged again from where Snake is now (its score was its own search's: live, Gatefront, a post kept on a
			// stale score while it no longer saw Snake was searched again every 1.5 s): one that fails now is no post to keep.
			std::optional<quiet::Post> curNow;
			if (cur) {
				quiet::Post c = *cur;
				c.mode = g_search.CurrentMode();
				const auto e = quiet::EvaluatePost(c, g_search.Scene(), LiveRays(a_f.player.chr), rules);
				if (e.why == quiet::PostWhy::kOk) {
					curNow = e;
				}
			}
			if (best) {
				g_retryNear = false;
				g_postMode = best->mode;
				if (!curNow || quiet::Better(*best, *curNow, rules)) {
					++g_posts;
					g_noPostAt = 0;
					g_fader.Want(best, inView, ::GetTickCount64());
					g_snakeSeenAt = ::GetTickCount64();
				} else {
					++g_kept;
					g_noPostAt = ::GetTickCount64();  // hers is still as good: not again every 1.5 s
					g_noPostFrom = snake;
				}
				g_targetUnseen = false;
				return;
			}
			// No candidate passed: Cover falls back to the Near rings, then the last post while it still sees Snake, else none.
			if (g_search.CurrentMode() == quiet::Mode::kCover && !g_retryNear) {
				g_retryNear = true;
				StartSearch(a_f, quiet::Mode::kNear, "no cover post: the near rings");
				return;
			}
			g_retryNear = false;
			if (cur && quiet::LineClear(cur->feet + Vec3{ 0, rules.sight, 0 }, snake + Vec3{ 0, kSnakeChest, 0 }, LiveRays(a_f.player.chr), rules.lineSlack)) {
				++g_kept;
				g_postMode = g_search.CurrentMode() == quiet::Mode::kNear && g_mode == quiet::Mode::kCover ? quiet::Mode::kCover : g_search.CurrentMode();
				g_noPostAt = ::GetTickCount64();  // nothing better here: not again every 1.5 s
				g_noPostFrom = snake;
				Log::Info("quiet: no post passed -- she keeps her last one (it still sees Snake); the next search in {} s or once Snake has moved {:.0f} m",
					kNoPostBackoffMs / 1000, kNoPostMoved);
				return;
			}
			++g_noPosts;
			g_noPostAt = ::GetTickCount64();
			g_noPostFrom = snake;
			Log::Info("quiet: no post ({} of {} candidates passed) -- she is unseen (no floor for her here: a cave, a corridor, a cliff); the next search in {} s "
			          "or once Snake has moved {:.0f} m",
				g_search.Passed(), g_search.Total(), kNoPostBackoffMs / 1000, kNoPostMoved);
			g_postMode = g_search.CurrentMode();
			if (cur) {
				g_fader.Want(std::nullopt, inView, ::GetTickCount64());
			}
		}

		void TickPosts(const Frame& a_f, const marking::Camera& a_cam, bool a_camOk)
		{
			const auto  rules = Rules();
			const auto  now = ::GetTickCount64();
			const auto& g = a_f.guest;
			const Vec3  snake = SnakeFeet(a_f);
			const bool  echo = (g.buddyFlags & proto::kBuddyAtPost) && g.buddyPostSeq == g_seq && g_seq != 0;
			const auto& cur = g_fader.Shown();
			const bool  inView = a_camOk && cur && InView(a_cam, cur->feet + Vec3{ 0, 0.8f, 0 });
			// The grace: while he rests she is the seat's (er/GraceRest.h); when he leaves, the seat becomes her post.
			const bool atGrace = Host::AtGrace();
			const bool seated = GraceRest::AtSeat();
			if (seated) {
				g_wasSeated = true;
			}
			// The rest hid her without seating her (a late echo, an Elden Ring dialog): when that ends she fades in at her post through the
			// cloak path (published again, echoed, faded in), never popped in at alpha 1 (review).
			{
				static bool hidWas = false;
				const bool  hid = GraceRest::Hide();
				if (hidWas && !hid && !g_wasSeated && g_fader.Shown()) {
					const quiet::Post p = *g_fader.Shown();
					g_fader.Want(p, false, now);
					Log::Info("quiet: the grace hid her without a seat -- she fades in at her post again");
				}
				hidWas = hid;
			}
			if (g_atGraceWas && !atGrace) {
				g_graceLeftAt = now;
				Vec3  feet;
				float heading = 0;
				if (g_wasSeated && GraceRest::LastSeat(feet, heading) && Finite(feet)) {
					quiet::Post seat;
					seat.mode = quiet::Mode::kNear;
					seat.feet = feet;
					seat.heading = heading;
					seat.score = -1e6f;  // any post found is better: her cloak fades her from here to it
					g_fader.Adopt(seat);
					g_postMode = quiet::Mode::kNear;
					g_everPosted = true;
					g_noPostAt = 0;
					Publish("(the grace seat she gets up from: her post until the next one, faded in view)");
				}
				g_wasSeated = false;
			}
			g_atGraceWas = atGrace;
			if (!Cfg().quiet.posts) {
				return;
			}
			if (atGrace || Host::Tarnished()) {
				// No searches while he rests or plays the Tarnished: her post is frozen.
				if (g_search.Running()) {
					g_search.Clear();
				}
			} else if (Live(a_f)) {
				const bool hostile = HostileNear(a_f, rules);
				const auto mode = g_move.Update(g_speed, a_f.dt, hostile, rules);
				if (mode != g_mode) {
					Log::Info("quiet: {} -> {} (Snake {:.1f} m/s{})", quiet::Name(g_mode), quiet::Name(mode), g_speed, hostile ? ", hostiles within 60 m" : "");
					g_mode = mode;
				}
				if (mode == quiet::Mode::kMoving) {
					if (g_search.Running()) {
						g_search.Clear();
					}
					if (cur || g_fader.Pending()) {
						++g_hidden;
						g_fader.Want(std::nullopt, inView, now);
						Log::Info("quiet: on the move -- unseen until Snake stops{}", inView ? " (her cloak fades her out in view)" : "");
					}
				} else if (!g_search.Running() && now - g_searchAt > kSearchCooldownMs && (!g_graceLeftAt || now - g_graceLeftAt > kAfterGraceMs)) {
					// The line from her post to Snake, twice a second.
					if (cur && now - g_snakeCheckAt >= kSnakeCheckMs) {
						g_snakeCheckAt = now;
						if (quiet::LineClear(cur->feet + Vec3{ 0, rules.sight, 0 }, snake + Vec3{ 0, kSnakeChest, 0 }, LiveRays(a_f.player.chr), rules.lineSlack)) {
							g_snakeSeenAt = now;
						}
					}
					quiet::Need n;
					n.havePost = cur.has_value() || g_fader.Pending().has_value();
					n.postMode = g_postMode;
					n.mode = mode;
					n.snakeFromCentre = LengthXZ(snake - g_searchCentre);
					n.lostSnake = cur && g_snakeSeenAt && now - g_snakeSeenAt > kLostSnakeMs;
					n.threatNear = cur && std::ranges::any_of(g_chrs, [&](const Chr& x) {
						return x.hostile && !x.c.dead && x.c.hp > 0 && Length(x.c.pos - cur->feet) < rules.threatNear;
					});
					n.targetUnseen = g_targetUnseen;
					// Nothing passed last time (indoors, a cave): not again every 1.5 s (live, Rampart Tower: 56 searches in 50 s, ~200 rays
					// each) -- after kNoPostBackoffMs, or once Snake has moved on, or hostiles came / went.
					const bool backoff = g_noPostAt && now - g_noPostAt < kNoPostBackoffMs && LengthXZ(snake - g_noPostFrom) < kNoPostMoved;
					if (const char* why = backoff ? nullptr : quiet::NeedSearch(n, rules)) {
						StartSearch(a_f, mode, why);
					}
				}
				if (g_search.Running()) {
					LARGE_INTEGER t0, t1, fq;
					::QueryPerformanceCounter(&t0);
					g_search.Step(rules.raysPerFrame, LiveRays(a_f.player.chr));
					::QueryPerformanceCounter(&t1);
					::QueryPerformanceFrequency(&fq);
					g_searchUs += static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
					if (g_search.Done()) {
						SearchDone(a_f, a_cam, a_camOk);  // (it may start the near rings' retry: that runs from the next frame)
					}
				}
			}
			const bool inViewNow = a_camOk && g_fader.Shown() && InView(a_cam, g_fader.Shown()->feet + Vec3{ 0, 0.8f, 0 });
			if (g_fader.Update(now, echo, inViewNow, rules)) {
				if (g_fader.Shown()) {
					Publish(inView ? "(after her cloak's fade-out)" : "");
				} else {
					Log::Info("quiet: no post published (on the move / none): buddyPost NaN");
				}
			}
			if (g_fader.EchoLate() && g_fader.Current() != quiet::Fader::Step::kWaitEcho) {
				++g_echoLate;
			}
		}

		quiet::Target TargetOf(const Chr& a_x)
		{
			quiet::Target t;
			t.handle = a_x.c.handle;
			t.feet = a_x.c.pos;
			t.chest = ChestOf(a_x);
			t.radius = a_x.body ? a_x.body->r : 0.5f;
			t.hostile = a_x.hostile;
			t.alive = !a_x.c.dead && a_x.c.hp > 0;
			t.rendered = a_x.rendered;
			if (t.hostile && t.alive) {
				t.boss = Boss(a_x);
				t.held = Cqc::IsHeld(a_x.c.handle);
				t.lifting = Fulton::IsLifting(a_x.c.handle);
				t.asleep = Combat::Asleep(a_x.c.chr);
				t.marked = Marking::IsMarked(a_x.c.handle);
			}
			return t;
		}

		void SetTarget(std::uint64_t a_handle, const std::string& a_name)
		{
			if (a_handle != g_target.load()) {
				++g_pickChanges;
				Log::Info("quiet: the pick (kActorQuietTarget) is {}", a_handle ? a_name : std::format("nobody ({})", g_lastWhy));
			}
			g_target = a_handle;
			g_targetName = a_handle ? a_name : std::string();
		}

		void TickPick(const Frame& a_f, const marking::Camera& a_cam, bool a_camOk, bool a_looking)
		{
			const auto now = ::GetTickCount64();
			if (!Cfg().quiet.orders) {
				// [Quiet] orders off: no pick is published, so the guest has nothing to order her at (MGSV's Fire stays greyed beside her).
				if (g_target.load()) {
					g_lastWhy = "[Quiet] orders off";
					SetTarget(0, "");
					g_latch.Clear();
				}
				g_threat = 0;
				g_targetUnseen = false;
				return;
			}
			if (now - g_pickAt < (a_looking ? kPickMs : kPickIdleMs)) {
				return;
			}
			g_pickAt = now;
			const auto  rules = Rules();
			const auto  sight = SightPoint();
			const Vec3  snake = SnakeFeet(a_f);
			const auto& g = a_f.guest;
			const bool  tranq = proto::BuddyQuietWeapon(g.buddyFlags) == proto::kQuietTranq;
			const bool  bino = (g.flags & proto::kGuestBinoculars) != 0;
			std::vector<quiet::Target> ts;
			ts.reserve(g_chrs.size());
			for (const auto& x : g_chrs) {
				ts.push_back(TargetOf(x));
			}
			const auto* ignore = a_f.player.chr;
			const auto  chrOf = [&](const quiet::Target& a_t) -> const Chr* {
				for (const auto& x : g_chrs) {
					if (x.c.handle == a_t.handle) {
						return &x;
					}
				}
				return nullptr;
			};
			const quiet::SightFn fromPost = [&](const quiet::Target& a_t) {
				const auto* x = chrOf(a_t);
				return sight && x && Sees(*sight, *x, ignore);
			};
			const quiet::SightFn fromView = [&](const quiet::Target& a_t) {
				const auto* x = chrOf(a_t);
				return a_camOk && x && Sees(a_cam.pos, *x, ignore);
			};
			g_last = a_camOk ? quiet::Pick(ts, { a_cam.pos, a_cam.fwd }, sight, snake, Cfg().actorRadius, tranq, bino, rules, fromPost, fromView) : quiet::PickResult{};
			++g_picks;
			const std::uint64_t fresh = g_last.index ? ts[*g_last.index].handle : 0;
			g_lastWhy = g_last.index                          ? std::string("picked")
			            : g_last.nearest && *g_last.nearest < g_chrs.size() ? std::format("{} -- {}", NameOf(g_chrs[*g_last.nearest].c), quiet::Name(g_last.why))
			                                                    : std::string(quiet::Name(g_last.why));
			// The enemy at the view's centre can't be seen from her post: search a post that sees him (§3.4).
			g_targetUnseen = !fresh && g_last.why == quiet::PickWhy::kNoSightPost && a_looking;
			bool fits = false;
			if (const auto held = g_latch.Handle(); held && held != fresh) {
				for (const auto& t : ts) {
					if (t.handle == held) {
						fits = quiet::LatchFits(t, sight, snake, Cfg().actorRadius, tranq, rules);
					}
				}
			}
			const auto published = g_latch.Update(fresh, fits, now, static_cast<std::uint64_t>(rules.latchMs));
			std::string name;
			for (const auto& x : g_chrs) {
				if (x.c.handle == published) {
					name = NameOf(x.c);
				}
			}
			SetTarget(published, name);
			// Cover me's threat pick (only while MGSV's cover mode is on): the nearest hostile to Snake within cover_threat_range she sees.
			std::uint64_t threat = 0;
			if ((g.buddyFlags & proto::kBuddyQuietCover) && sight) {
				const Chr* best = nullptr;
				float      bestD = rules.coverThreatRange;
				for (std::size_t i = 0; i < g_chrs.size(); ++i) {
					const float d = Length(g_chrs[i].c.pos - snake);
					if (d <= bestD && quiet::LatchFits(ts[i], sight, snake, Cfg().actorRadius, tranq, rules)) {
						best = &g_chrs[i];
						bestD = d;
					}
				}
				if (best && Sees(*sight, *best, ignore)) {
					threat = best->c.handle;
				}
			}
			g_threat = threat;
		}

		void TickSpotting(const Frame& a_f)
		{
			const auto now = ::GetTickCount64();
			const auto rules = Rules();
			if (!Cfg().quiet.spot || now - g_spotAt < static_cast<ULONGLONG>(rules.spotEveryMs)) {
				return;
			}
			g_spotAt = now;
			const auto sight = SightPoint();
			if (!sight) {
				return;
			}
			const Vec3 snake = SnakeFeet(a_f);
			std::vector<std::uint64_t> seen;
			std::vector<const Chr*>    cands;
			for (const auto& x : g_chrs) {
				const auto t = TargetOf(x);
				if (!quiet::LatchFits(t, sight, snake, Cfg().actorRadius, false, rules) || Length(x.c.pos - *sight) > rules.spotRange) {
					g_spotter.Forget(x.c.handle);  // dead, gone, not hostile (any more), a boss: never marked
					continue;
				}
				cands.push_back(&x);
			}
			// At most 4 rays a pass, round robin; the ones not looked at keep their marks until they lapse.
			for (std::size_t k = 0; k < cands.size() && k < 4; ++k) {
				const auto* x = cands[(g_spotNext + k) % cands.size()];
				if (Sees(*sight, *x, a_f.player.chr)) {
					seen.push_back(x->c.handle);
				}
			}
			g_spotNext += 4;
			const auto fresh = g_spotter.Update(seen, now, rules);
			for (const auto h : seen) {
				Marking::Provisional(h, g_spotter.Until(h));
			}
			g_spotted += fresh.size();
			if (const auto text = g_spotText.Update(fresh.size(), g_spotter.Count(now), now, rules.spotTextEvery); !text.empty() && Cfg().quiet.orderText) {
				++g_texts;
				Hud::Notify(text, Hud::Tone::kInfo);
			}
		}

		void TickReticle(const Frame& a_f, const marking::Camera& a_cam, bool a_camOk, bool a_show)
		{
			std::vector<Marker> ms;
			const auto          target = g_target.load();
			if (a_show && a_camOk && target && Cfg().quiet.indicator) {
				for (const auto& x : g_chrs) {
					if (x.c.handle != target) {
						continue;
					}
					const bool marked = (a_f.guest.buddyFlags & proto::kBuddyQuietMarked) != 0;
					Marker     m;
					m.shape = Marker::Shape::kRing;
					m.pos = ChestOf(x);
					m.normal = a_cam.fwd * -1.0f;
					m.radius = std::clamp(Length(m.pos - a_cam.pos) * 0.02f, 0.25f, 1.5f);
					m.inner = 0.8f;
					std::copy(std::begin(marked ? kQuietColour : kWhite), std::end(marked ? kQuietColour : kWhite), m.color);
					m.slack = 1e4f;  // seen through what is in between: the player is told who she would take
					ms.push_back(m);
				}
			}
			RenderMarkers::Set(RenderMarkers::Owner::kQuiet, std::move(ms));
		}

		void Reset(const char* a_why)
		{
			if (g_target.load() || g_fader.Shown() || g_search.Running()) {
				Log::Info("quiet: {} -- her post, the pick and the spotting are cleared", a_why);
			}
			g_target = 0;
			g_threat = 0;
			g_targetName.clear();
			g_latch.Clear();
			g_search.Clear();
			g_fader.Reset();
			g_move.Reset();
			g_mode = g_postMode = quiet::Mode::kNone;
			g_everPosted = false;
			g_retryNear = false;
			g_spotter.Clear();
			g_targetUnseen = false;
			g_havePe = false;
			g_wasSeated = false;
			RenderMarkers::Set(RenderMarkers::Owner::kQuiet, {});
		}

		// kGEvQuietShot Hit: her round in Elden Ring, from her post's sight point.
		void ApplyHit(const Frame& a_f, std::uint64_t a_handle, proto::QuietWeapon a_weapon, std::uint32_t a_attack, float a_share, std::string_view a_from)
		{
			const auto now = ::GetTickCount64();
			if (auto it = g_lastHit.find(a_handle); it != g_lastHit.end() && now - it->second < kHitDedupeMs) {
				Log::Info("quiet: a second Hit on {:#x} within {} ms -- one effect per order", a_handle, kHitDedupeMs);
				return;
			}
			g_lastHit[a_handle] = now;
			++g_hits;
			const auto rules = Rules();
			// The rifle: the attack id's, else the event's, else the guest's buddyFlags (her equipment); still unknown = refused, never
			// lethal (review: a tranq order could kill).
			auto       w = quiet::WeaponOfAttack(a_attack);
			if (w == proto::kQuietWeaponUnknown) {
				w = a_weapon;
			}
			if (w == proto::kQuietWeaponUnknown) {
				w = proto::BuddyQuietWeapon(a_f.guest.buddyFlags);
			}
			const auto chrs = Around(a_f);
			const Chr* x = nullptr;
			for (const auto& y : chrs) {
				if (y.c.handle == a_handle) {
					x = &y;
				}
			}
			quiet::ShotTarget t;
			const auto        sight = SightPoint();
			bool              clear = false;
			if (x) {
				t.present = true;
				t.hostile = x->hostile;
				t.alive = !x->c.dead && x->c.hp > 0;
				t.boss = Boss(*x);
				t.maxHp = x->c.maxHp;
				clear = sight && t.hostile && t.alive && !t.boss && Sees(*sight, *x, a_f.player.chr);
			}
			const auto shot = quiet::ShotEffect(w, t, a_share, sight.has_value(), clear, rules, Combat::CurrentRules(), Cqc::CurrentRules(), Combat::CurrentMelee());
			++g_acts[static_cast<std::size_t>(shot.act)];
			const auto name = x ? NameOf(x->c) : std::format("ER {:#x}", a_handle);
			Log::Info("quiet: {} hit {} -> {}: damage {}{}, poise {:.0f} lv {}, sleep {} / {:.2f} of his resistance{} ({}; from her post's sight point{})", a_from, name,
				quiet::Name(shot.act), shot.hit.damage, shot.hit.nonLethal ? " (never below 1 HP)" : "", shot.hit.poise, shot.hit.dmgLevel, shot.hit.sleep,
				shot.hit.sleepShare, a_share > 0 ? std::format(", MGSV's share {:.3f}", a_share) : std::string(), quiet::WeaponName(w),
				sight ? std::format(" ({:.2f}, {:.2f}, {:.2f}), {:.1f} m", sight->x, sight->y, sight->z, x ? Length(ChestOf(*x) - *sight) : 0.0f) : std::string(": none"));
			switch (shot.act) {
			case quiet::ShotAct::kLethal:
			case quiet::ShotAct::kHeavy:
			case quiet::ShotAct::kSleep: {
				const float r = x->body ? x->body->r : 0.5f;
				Vec3        dir = ChestOf(*x) - *sight;
				dir = Length(dir) > 1e-3f ? Normalize(dir) : Vec3{ 0, 0, 1 };
				const Vec3 from = ChestOf(*x) - dir * (r + 0.8f);
				if (Combat::HitCharacter(a_f, a_handle, from, r + 1.5f, shot.hit, std::format("Quiet's {}", quiet::WeaponName(w)))) {
					++g_hitsLanded;
				} else {
					Log::Info("quiet: the hit on {} -- out of reach (or [Combat] damage is off)", name);
				}
				break;
			}
			case quiet::ShotAct::kBlocked:
				if (Cfg().quiet.orderText) {
					Hud::Notify(quiet::kNoClearShot, Hud::Tone::kWarn);
				}
				break;
			case quiet::ShotAct::kNoPost:
				if (Cfg().quiet.orderText) {
					Hud::Notify(quiet::kNoPostShot, Hud::Tone::kWarn);
				}
				break;
			case quiet::ShotAct::kNotHostile:
				Log::Warn("quiet: Hit on {} -- NOT a hostile Elden Ring enemy (a friend, a neutral, a merchant, a summon): nothing done", name);
				break;
			case quiet::ShotAct::kBoss:
				Log::Warn("quiet: Hit on {} -- a boss ([Quiet] bosses = off): nothing done", name);
				break;
			case quiet::ShotAct::kUnknownRifle:
				Log::Warn("quiet: Hit on {} with MGSV attack {} -- her rifle is unknown (not 290-292, no weapon from the guest): nothing done", name, a_attack);
				break;
			default:
				break;
			}
		}
	}

	void Tick(const Frame& a_f)
	{
		bool out = Cfg().quiet.enabled && Out(a_f);
		// A guest frame that drops for a moment (live 2026-11-04: one-frame gaps while the stand-in served a console command) is no
		// dismissal: her post and the pick are kept unless she is gone for kOutLostMs, another buddy is out, or the layer is off.
		const auto  nowOut = ::GetTickCount64();
		const auto& gb = a_f.guest;
		const bool  other = a_f.guestReady && gb.buddyType != Buddy::kQuiet;
		if (out) {
			g_outLostAt = 0;
		} else if (g_wasOut && Cfg().quiet.enabled && !other && !a_f.loading) {
			if (!g_outLostAt) {
				g_outLostAt = nowOut;
			}
			if (nowOut - g_outLostAt < kOutLostMs) {
				return;  // (nothing runs this frame; her state is kept)
			}
		}
		if (!out) {
			if (g_wasOut) {
				g_wasOut = false;
				g_outLostAt = 0;
				Reset(Cfg().quiet.enabled ? "Quiet isn't out (dismissed, another buddy, a load)" : "[Quiet] off");
			}
			return;
		}
		if (!g_wasOut) {
			g_wasOut = true;
			Log::Info("quiet: Quiet is out -- the host's layer runs (posts {}, spotting {}, reticle {}, texts {})", Cfg().quiet.posts ? "on" : "off",
				Cfg().quiet.spot ? "on" : "off", Cfg().quiet.indicator ? "on" : "off", Cfg().quiet.orderText ? "on" : "off");
		}
		const auto now = ::GetTickCount64();
		// Snake's speed (the anchor's step a frame, smoothed).
		const Vec3 pe = SnakeFeet(a_f);
		if (g_havePe && a_f.dt > 0.0f) {
			const Vec3  d = pe - g_lastPe;
			const float v = LengthXZ(d) / a_f.dt;
			if (LengthXZ(d) < 5.0f) {  // a teleport or a world shift is no speed
				g_speed += (v - g_speed) * std::min(1.0f, a_f.dt * 6.0f);
			}
		}
		g_lastPe = pe;
		g_havePe = true;
		if (now - g_chrsAt >= kCharsMs || g_chrs.empty()) {
			g_chrsAt = now;
			g_chrs = Around(a_f, &g_phantoms);
		}
		marking::Camera cam;
		const bool      camOk = ReadCamera(cam);
		TickPosts(a_f, cam, camOk);
		// The pick, the spotting, the reticle: while she plays (not at the grace, not in Tarnished mode).
		const bool lb = (InputBridge::LastButtons() & kLb) != 0;
		const bool bino = (a_f.guest.flags & proto::kGuestBinoculars) != 0;
		if (lb && !g_lbDown) {
			g_lbDownAt = now;
			g_hintSaid = false;
		}
		g_lbDown = lb;
		if (lb || bino) {
			g_lookAt = now;
		}
		const bool looking = g_lookAt && now - g_lookAt < kLookAfterMs;
		if (Live(a_f) && !Host::AtGrace()) {
			TickPick(a_f, cam, camOk, looking);
			TickSpotting(a_f);
			if (lb && !g_hintSaid && now - g_lbDownAt >= kHintMs && !(a_f.guest.buddyFlags & proto::kBuddyQuietMarked) && Cfg().quiet.orderText && Cfg().quiet.orders &&
				!(a_f.guest.flags & (proto::kGuestInMenu | proto::kGuestOnVehicle))) {
				g_hintSaid = true;
				++g_hints;
				Hud::Notify(g_targetUnseen ? quiet::kMovingForShot : quiet::kMarkHint, Hud::Tone::kInfo);
			}
			TickReticle(a_f, cam, camOk, (lb || bino) && Cfg().quiet.orders);
		} else {
			if (g_target.load()) {
				g_lastWhy = Host::AtGrace() ? "at the grace" : "not playing";
				SetTarget(0, "");
				g_latch.Clear();
			}
			g_threat = 0;
			RenderMarkers::Set(RenderMarkers::Owner::kQuiet, {});
		}
	}

	void Fill(proto::HostState& a_s, const Anchor& a_a)
	{
		for (int i = 0; i < 3; ++i) {
			a_s.buddyPost[i] = NAN;
		}
		a_s.buddyPostYaw = 0.0f;
		a_s.buddyPostSeq = g_seq;
		if (!Cfg().quiet.enabled) {
			return;
		}
		if (!Cfg().quiet.posts) {
			// The layer needs posts (review): without them she has no sight point, so every pick, Fire and grace seat would be refused while
			// the guest took her orders over -- worse than off. She stays 0.8.0's Quiet then.
			static bool said = false;
			if (!said) {
				said = true;
				Log::Warn("quiet: [Quiet] enabled without posts -- the layer stays off (posts are required)");
			}
			return;
		}
		a_s.flags |= proto::kHostQuietLayer;
		const auto& p = g_fader.Shown();
		if (!p || !g_wasOut) {
			return;
		}
		const Vec3 m = a_a.PointToMgsv(p->feet);
		a_s.buddyPost[0] = m.x;
		a_s.buddyPost[1] = m.y;
		a_s.buddyPost[2] = m.z;
		const Vec3 dir = a_a.DirToMgsv(Vec3{ std::sin(p->heading), 0, std::cos(p->heading) });
		a_s.buddyPostYaw = a_a.axes.mgsvYawSign * Heading(dir);
	}

	Placement Place()
	{
		Placement pl;
		const auto& f = Host::Current();
		const auto& g = f.guest;
		if (!Cfg().quiet.enabled || !g_wasOut || g.buddyType != Buddy::kQuiet) {
			return pl;
		}
		if ((g.buddyFlags & proto::kBuddyCloaked) && Cfg().quiet.cloak == "hide") {
			pl.kind = Placement::Kind::kHidden;
			pl.why = "cloaked (MGSV draws nothing)";
			return pl;
		}
		if (GraceRest::AtSeat()) {
			pl.kind = Placement::Kind::kAt;
			pl.feet = GraceRest::Seat();
			pl.alpha = GraceRest::SeatAlpha();  // faded in when she settled after the black (her cloak)
			pl.why = "at the grace seat";
			return pl;
		}
		if (GraceRest::Hide()) {
			pl.kind = Placement::Kind::kHidden;
			pl.why = "the grace (no echo inside the black, or a dialog)";
			return pl;
		}
		if (!Cfg().quiet.posts || !g_everPosted) {
			return pl;  // MGSV's own place, on Elden Ring's ground (the far-ground rule)
		}
		const auto& p = g_fader.Shown();
		if (!p || g_fader.Alpha() <= 0.0f) {
			pl.kind = Placement::Kind::kHidden;
			pl.why = p ? "her cloak (a post change)" : "no post (on the move)";
			return pl;
		}
		if (Host::Tarnished() && f.player && LengthXZ(game::Position(f.player) - p->feet) > 60.0f) {
			pl.kind = Placement::Kind::kHidden;
			pl.why = "Tarnished mode, far from her post";
			return pl;
		}
		pl.kind = Placement::Kind::kAt;
		pl.feet = p->feet;
		pl.alpha = g_fader.Alpha();
		pl.why = "at her post";
		return pl;
	}

	std::uint64_t TargetHandle() { return g_target.load(); }
	std::uint64_t ThreatHandle() { return g_threat.load(); }

	void OnWorldShift(const Vec3& a_d)
	{
		g_fader.Shift(a_d);
		g_search.Shift(a_d);
		g_searchCentre += a_d;
		g_lastPe += a_d;
	}

	void OnOrder(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (a_e.b != Buddy::kQuiet || !Cfg().quiet.enabled) {
			return;
		}
		// Take aim / Fire with no pick: the guest refuses (kGEvQuietShot Refused); the reticle tells the player who it would take.
		if ((a_e.a >= 1 && a_e.a <= 3) && !g_target.load()) {
			Log::Info("quiet: order {} with no pick ({})", a_e.a, g_lastWhy);
		}
	}

	void OnShot(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		static constexpr const char* kPhases[] = { "ordered", "hit", "released", "refused", "lost" };
		static constexpr const char* kLost[] = { "?", "MGSV never fired", "the target gone", "cancelled" };
		const auto          phase = static_cast<std::uint32_t>(proto::QuietShotPhase(a_e.a));
		const auto          weapon = proto::QuietShotWeapon(a_e.a);
		const auto          attack = proto::QuietShotAttackId(a_e.a);
		const std::uint64_t handle = a_e.pad[0] | (static_cast<std::uint64_t>(a_e.pad[1]) << 32);
		const bool          lost = phase == proto::kQuietLost;
		if (phase < g_phases.size()) {
			++g_phases[phase];
		}
		Log::Info("quiet: kGEvQuietShot {} ({}, MGSV attack {}, {}) on ER {:#x}, Quiet at {:.2f},{:.2f},{:.2f} in MGSV{}", phase < std::size(kPhases) ? kPhases[phase] : "?",
			quiet::WeaponName(weapon), attack, lost ? std::format("reason: {}", a_e.b < std::size(kLost) ? kLost[a_e.b] : "?") : std::format("share {:.4f}", a_e.b / 10000.0),
			handle, a_e.v0[0], a_e.v0[1], a_e.v0[2], Cfg().quiet.enabled ? "" : " -- logged only ([Quiet] enabled = false)");
		if (!Cfg().quiet.enabled) {
			return;
		}
		if (phase == proto::kQuietHit) {
			if (!handle) {
				Log::Info("quiet: a Hit with no handle -- nothing done");
				return;
			}
			ApplyHit(a_f, handle, weapon, attack, a_e.b / 10000.0f, "her round");
			return;
		}
		if (const auto text = quiet::ShotText(phase, lost ? a_e.b : 0); !text.empty() && Cfg().quiet.orderText && Cfg().quiet.orders) {
			++g_texts;
			Hud::Notify(text, Hud::Tone::kWarn);
		}
	}

	std::string Describe()
	{
		const auto& c = Cfg().quiet;
		const auto  now = ::GetTickCount64();
		const auto& p = g_fader.Shown();
		const auto  pl = Place();
		const auto  a = [](quiet::ShotAct x) { return g_acts[static_cast<std::size_t>(x)]; };
		return std::format(
			"Quiet ([Quiet] enabled {}, posts {}, orders {}, spot {}, indicator {}, order_text {}): {}; mode {} (Snake {:.1f} m/s); post {} (buddyPostSeq {}, the guest {} {}), "
			"drawn: {} ({}{}); searches {} ({} posts, {} kept, {} none, {} hidden on the move, {} echoes waited out), {}; pick {} ({} picks, {} changes; "
			"threat {:#x}); spotted {} (now {}), texts {}, hints {}; shots: ordered {} hit {} released {} refused {} lost {}; effects: lethal {} heavy {} sleep {}, "
			"refused not hostile {} boss {} dead {} gone {}, no post {}, blocked {}; landed {} of {}; rays {}{}",
			c.enabled ? "on" : "off", c.posts ? "on" : "off", c.orders ? "on" : "off", c.spot ? "on" : "off", c.indicator ? "on" : "off", c.orderText ? "on" : "off",
			g_wasOut ? "out" : "not out", quiet::Name(g_mode), g_speed,
			p ? std::format("({:.2f}, {:.2f}, {:.2f}) {} {:.0f} m @{:.0f} deg", p->feet.x, p->feet.y, p->feet.z, quiet::Name(p->mode), p->ring, p->deg) : std::string("none"),
			g_seq, Host::Current().guest.buddyPostSeq, (Host::Current().guest.buddyFlags & proto::kBuddyAtPost) ? "holds her there" : "doesn't hold her",
			pl.kind == Placement::Kind::kAt ? std::format("at ({:.2f}, {:.2f}, {:.2f})", pl.feet.x, pl.feet.y, pl.feet.z)
			: pl.kind == Placement::Kind::kHidden ? std::string("hidden")
			                                      : std::string("MGSV's place"),
			*pl.why ? pl.why : "MGSV's place", pl.alpha < 1.0f ? std::format(", alpha {:.2f}", pl.alpha) : std::string(), g_searches, g_posts, g_kept, g_noPosts,
			g_hidden, g_echoLate, g_search.Running() || g_search.Done() ? g_search.Summary() : std::string("no search yet"),
			g_target.load() ? g_targetName : std::format("nobody ({})", g_lastWhy), g_picks, g_pickChanges, g_threat.load(), g_spotted, g_spotter.Count(now), g_texts,
			g_hints, g_phases[0], g_phases[1], g_phases[2], g_phases[3], g_phases[4], a(quiet::ShotAct::kLethal), a(quiet::ShotAct::kHeavy), a(quiet::ShotAct::kSleep),
			a(quiet::ShotAct::kNotHostile), a(quiet::ShotAct::kBoss), a(quiet::ShotAct::kDead), a(quiet::ShotAct::kGone), a(quiet::ShotAct::kNoPost),
			a(quiet::ShotAct::kBlocked), g_hitsLanded, g_hits, g_rays, g_testBoss ? "; TEST: every pick treated as a boss" : "");
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		auto& c = Cfg().quiet;
		if (a_w.size() < 2 || a_w[1] == "status") {
			return Describe();
		}
		if (a_w[1] == "on" || a_w[1] == "off") {
			c.enabled = c.posts = a_w[1] == "on";  // the layer needs posts (review)
			return Describe();
		}
		if (a_w[1] == "all" && a_w.size() > 2 && (a_w[2] == "on" || a_w[2] == "off")) {
			// The whole layer for a test run (not saved): enabled, posts, spotting, reticle, texts.
			c.enabled = c.posts = c.orders = c.spot = c.indicator = c.orderText = a_w[2] == "on";
			return Describe();
		}
		if (a_w[1] == "orders" && a_w.size() > 2 && (a_w[2] == "on" || a_w[2] == "off")) {
			c.orders = a_w[2] == "on";  // her pick, the reticle and the LB hint (Fire / Take aim on Elden Ring enemies), for the session
			return Describe();
		}
		if (a_w[1] == "test" && a_w.size() > 2 && a_w[2] == "boss") {
			g_testBoss = !g_testBoss;
			return std::format("quiet: test boss {} (every character is treated as a boss by the pick and the shot: nothing is picked or hit)",
				g_testBoss ? "ON" : "off");
		}
		if (a_w[1] == "record" && a_w.size() > 2) {
			g_record = a_w[2] == "on";
			return std::format("quiet: {} (PhantomRingQuietRays-N.json next to the DLL, one per search)", g_record ? "every post search's rays recorded" : "no recording");
		}
		if (a_w[1] == "pick") {
			// U14's dry run: every character around and why he is (not) her pick. No order, no shot, no mark.
			if (!a_f.player || !a_f.inGame) {
				return "quiet pick: not in the world";
			}
			marking::Camera cam;
			if (!ReadCamera(cam)) {
				return "quiet pick: no camera";
			}
			const auto  rules = Rules();
			const auto  chrs = Around(a_f);
			const Vec3  snake = SnakeFeet(a_f);
			auto        sight = SightPoint();
			const bool  assumed = !sight;
			if (!sight) {
				sight = snake + Vec3{ 0, rules.sight, 0 };  // no post: tested from Snake's own spot (said so)
			}
			const bool  tranq = proto::BuddyQuietWeapon(a_f.guest.buddyFlags) == proto::kQuietTranq;
			const bool  bino = (a_f.guest.flags & proto::kGuestBinoculars) != 0;
			std::string out = std::format("quiet pick (dry run: no order, no shot, no mark) from {} ({:.2f}, {:.2f}, {:.2f}); view ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, "
			                              "{:.2f}, {:.2f}); cone {:.0f} deg{}{}\n",
				assumed ? "Snake's spot (she has no post)" : "her post's sight point", sight->x, sight->y, sight->z, cam.pos.x, cam.pos.y, cam.pos.z, cam.fwd.x,
				cam.fwd.y, cam.fwd.z, bino ? rules.coneBinocularsDeg : rules.coneDeg, tranq ? ", Guilty Butterfly (no sleepers)" : "",
				g_testBoss ? ", TEST BOSS" : "");
			std::size_t n = 0;
			// The PlayerIns NPCs (an NPC phantom such as White Mask Varre, a summoned spirit's or a co-op player's character): never even
			// read as candidates (Around leaves them out); listed here so the dry run shows them refused.
			for (const auto& c : game::Characters(128)) {
				if (!c.isPlayer || c.chr == a_f.player.chr || !Finite(c.pos) || Length(c.pos - snake) > 80.0f) {
					continue;
				}
				marking::Camera v = cam;
				const Vec3      d = c.pos + Vec3{ 0, 1.2f, 0 } - v.pos;
				const float     off = std::acos(std::clamp(Dot(Normalize(d), v.fwd), -1.0f, 1.0f));
				out += std::format("  P {} d {:.1f} m, {:.1f} deg off -- a PlayerIns (an NPC phantom, a spirit, a player): never a candidate\n", NameOf(c),
					Length(c.pos - snake), off * 180.0f / 3.14159265f);
			}
			for (const auto& x : chrs) {
				if (Length(x.c.pos - snake) > 80.0f || n >= 40) {
					continue;
				}
				++n;
				auto  t = TargetOf(x);
				float off = 0;
				auto  why = quiet::Screen(t, { cam.pos, cam.fwd }, sight, snake, Cfg().actorRadius, tranq, bino, rules, &off);
				std::string rays;
				if (why == quiet::PickWhy::kOk || why == quiet::PickWhy::kOffCentre) {
					const bool post = Sees(*sight, x, a_f.player.chr);
					const bool view = Sees(cam.pos, x, a_f.player.chr);
					rays = std::format(" (seen from her post: {}, from the camera: {})", post ? "yes" : "NO", view ? "yes" : "NO");
					if (why == quiet::PickWhy::kOk && !post) {
						why = quiet::PickWhy::kNoSightPost;
					} else if (why == quiet::PickWhy::kOk && !view) {
						why = quiet::PickWhy::kNoSightView;
					}
				}
				out += std::format("  {} {} d {:.1f} m{}, {:.1f} deg off -- {}{}\n", x.c.isPlayer ? "P" : "E", NameOf(x.c), Length(x.c.pos - snake),
					x.hostile ? "" : " (not hostile)", off * 180.0f / 3.14159265f, why == quiet::PickWhy::kOk ? "CANDIDATE" : quiet::Name(why), rays);
			}
			return out + std::format("{} characters; the pick now: {}", n, g_target.load() ? g_targetName : std::format("nobody ({})", g_lastWhy));
		}
		if (a_w[1] == "post") {
			const bool none = a_w.size() > 2 && a_w[2] == "none";
			if (none) {
				// As on the move: her cloak fades her out if she is in view (the fade's test), then the post rule finds her a new one.
				marking::Camera cam;
				const auto&     cur = g_fader.Shown();
				const bool      inView = cur && ReadCamera(cam) && InView(cam, cur->feet + Vec3{ 0, 0.8f, 0 });
				g_fader.Want(std::nullopt, inView, ::GetTickCount64());
				return std::format("quiet: her post cleared ({}): unseen until the post rule finds her another", inView ? "in view: her cloak fades her out" : "out of view: at once");
			}
			if (!Out(a_f)) {
				return "quiet post: Quiet isn't out (or [Quiet] is off)";
			}
			StartSearch(a_f, HostileNear(a_f, Rules()) ? quiet::Mode::kCover : quiet::Mode::kNear, "console");
			return std::format("quiet: a post search started ({})", quiet::Name(g_search.CurrentMode()));
		}
		if (a_w[1] == "survey") {
			// U13: the design's rule on every candidate of both modes right now (no early stop), every ray written out.
			if (!a_f.player || !a_f.inGame) {
				return "quiet survey: not in the world";
			}
			marking::Camera cam;
			if (!ReadCamera(cam)) {
				return "quiet survey: no camera";
			}
			g_chrs = Around(a_f);
			g_chrsAt = ::GetTickCount64();
			const auto  rules = Rules();
			const auto  scene = Scene(a_f, cam, rules);
			const auto  label = a_w.size() > 2 ? a_w[2] : std::string("survey");
			std::string out;
			const bool  was = g_record;
			g_record = true;
			for (const auto mode : { quiet::Mode::kCover, quiet::Mode::kNear }) {
				RecordBegin(scene, std::format("{} {}", label, quiet::Name(mode)));
				const auto  ray = LiveRays(a_f.player.chr);
				int         rays = 0;
				std::map<float, std::pair<int, int>> rings;
				std::array<int, static_cast<std::size_t>(quiet::PostWhy::kCount)> why{};
				std::optional<quiet::Post> best;
				LARGE_INTEGER t0, t1, fq;
				::QueryPerformanceCounter(&t0);
				std::string spots;
				for (const auto& cand : quiet::PostCandidates(scene, mode, rules)) {
					const auto p = quiet::EvaluatePost(cand, scene, ray, rules, &rays);
					++why[static_cast<std::size_t>(p.why)];
					auto& rg = rings[p.ring];
					++rg.second;
					if (p.why == quiet::PostWhy::kOk) {
						++rg.first;
						if (!best || p.score > best->score) {
							best = p;
						}
					}
					spots += std::format("{}{{\"ring\": {:.0f}, \"deg\": {:.0f}, \"feet\": {}, \"why\": \"{}\", \"seen\": {}, \"score\": {:.3f}, \"forward\": {}}}",
						spots.empty() ? "\n  " : ",\n  ", p.ring, p.deg, J(p.feet), quiet::Name(p.why), p.seen, p.why == quiet::PostWhy::kOk ? p.score : 0.0f,
						p.forward ? "true" : "false");
				}
				::QueryPerformanceCounter(&t1);
				::QueryPerformanceFrequency(&fq);
				const double us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
				std::string ringText;
				for (const auto& [r, pc] : rings) {
					ringText += std::format("{}{:.0f} m: {} of {}", ringText.empty() ? "" : ", ", r, pc.first, pc.second);
				}
				std::string whyText;
				for (std::size_t i = 1; i < why.size(); ++i) {
					if (why[i]) {
						whyText += std::format("{}{} {}", whyText.empty() ? "" : ", ", quiet::Name(static_cast<quiet::PostWhy>(i)), why[i]);
					}
				}
				RecordEnd(std::format(" \"mode\": \"{}\",\n \"spots\": [{}\n ],\n \"best\": {}", quiet::Name(mode), spots,
					best ? std::format("{{\"ring\": {:.0f}, \"deg\": {:.0f}, \"feet\": {}, \"seen\": {}, \"score\": {:.3f}}}", best->ring, best->deg, J(best->feet),
							   best->seen, best->score)
						 : std::string("null")));
				out += std::format("{} ({} hostiles within 60 m): passed {} -- {}; refused: {}; best {}; {} rays in {:.0f} us ({:.1f} us a ray)\n", quiet::Name(mode),
					scene.hostiles.size(), ringText, best ? "ok" : "NONE", whyText, best ? PostLine(*best, scene.snake) : std::string("none"), rays, us,
					rays ? us / rays : 0.0);
			}
			g_record = was;
			return out;
		}
		if (a_w[1] == "shoot") {
			// U6 / the effects: a Hit as kGEvQuietShot brings it (the pick by default), from her post's sight point.
			std::uint64_t handle = g_target.load();
			auto          w = proto::kQuietLethal;
			for (std::size_t i = 2; i < a_w.size(); ++i) {
				if (a_w[i].starts_with("0x")) {
					handle = std::strtoull(a_w[i].c_str(), nullptr, 16);
				} else if (a_w[i] == "tranq") {
					w = proto::kQuietTranq;
				} else if (a_w[i] == "heavy") {
					w = proto::kQuietHeavy;
				}
			}
			if (!handle) {
				return std::format("quiet shoot: no pick ({}) and no handle given", g_lastWhy);
			}
			static constexpr std::uint32_t kAttack[] = { 290, 291, 292 };
			static constexpr float         kShare[] = { 0.9615f, 0.4615f, 1.4615f };
			g_lastHit.erase(handle);
			ApplyHit(a_f, handle, w, kAttack[w], kShare[w], "a console shot");
			return Describe();
		}
		if (a_w[1] == "set" && a_w.size() > 3) {
			const auto& k = a_w[2];
			const auto& v = a_w[3];
			const float f = std::strtof(v.c_str(), nullptr);
			const bool  b = v == "on" || v == "true" || v == "1";
			if (k == "enabled") c.enabled = b;
			else if (k == "posts") c.posts = b;
			else if (k == "spot") c.spot = b;
			else if (k == "indicator") c.indicator = b;
			else if (k == "order_text") c.orderText = b;
			else if (k == "post_drawable") c.postDrawable = b;
			else if (k == "target_range") c.targetRange = std::max(0.0f, f);
			else if (k == "target_cone_deg") c.targetConeDeg = std::clamp(f, 0.5f, 90.0f);
			else if (k == "post_height_max") c.postHeightMax = f;
			else if (k == "post_height_min") c.postHeightMin = f;
			else if (k == "post_rays_per_frame") c.postRaysPerFrame = std::max(4, static_cast<int>(f));
			else if (k == "fade_ms") c.fadeMs = std::max(0, static_cast<int>(f));
			else if (k == "move_hide_speed") c.moveHideSpeed = std::max(0.0f, f);
			else if (k == "cloak") c.cloak = v == "shimmer" ? "shimmer" : "hide";
			else return std::format("quiet set: unknown key {}", k);
			return Describe();
		}
		return "quiet [status | on | off | all on|off | pick | test boss | post [now|none] | shoot [0xHANDLE] [lethal|tranq|heavy] | survey [label] | record on|off | "
		       "set <key> <value>]";
	}
}
