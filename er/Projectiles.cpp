#include "Projectiles.h"

#include "Config.h"
#include "Ini.h"
#include "Log.h"
#include "ProjectileLogic.h"
#include "Puppet.h"
#include "RenderAssets.h"
#include "RenderScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include <windows.h>

namespace phantom::er::Projectiles
{
	namespace
	{
		constexpr double kLearnWithinMs = 1500.0;  // the guest tags for ~0.5 s; the list can trail the event by a few frames
		constexpr int    kMaxTries = 8;            // tagged lists a release may show without two that fly like it
		constexpr int    kReplaceAfter = 2;        // later learns in a row agreeing on other meshes before a template goes

		struct Release
		{
			projectile::Pending                 pending;
			Vec3                                dirMgsv;
			std::optional<projectile::Template> candidate;  // its first tagged list, until a later one shows it flew
			std::uint64_t                       candidateFrame{ 0 };
			int                                 tries{ 0 };
		};

		struct Challenger
		{
			std::vector<std::uint64_t> signature;
			int                        count{ 0 };
		};

		std::mutex                                                   g_lock;
		std::vector<Release>                                         g_pending;
		std::map<std::uint64_t, projectile::Template>                g_templates;
		std::map<std::uint64_t, Challenger>                          g_challengers;
		std::array<std::vector<Flying>, std::size_t(Source::kCount)> g_flying;
		std::map<std::uint64_t, projectile::Pose>                    g_poses;      // by KeyOf(source, copy id)
		std::set<std::uint64_t>                                      g_placedNow;  // copies placed in the last Apply ...
		std::uint64_t                                                g_placedFrame{ 0 };  // ... of this Elden Ring frame
		std::uint64_t g_releases{ 0 }, g_learned{ 0 }, g_kept{ 0 }, g_replaced{ 0 }, g_rejected{ 0 }, g_unconfirmed{ 0 }, g_unclaimed{ 0 }, g_listsWithTags{ 0 },
			g_placed{ 0 }, g_handedBack{ 0 };

		bool IsThrow(std::uint64_t a_key) { return (a_key >> 32) - 1 == std::uint64_t(Source::kThrow); }

		std::string KeyName(std::uint64_t a_key) { return std::format("{} {}", IsThrow(a_key) ? "throw equip" : "round bullet", a_key & 0xFFFFFFFF); }

		// (0.7.0) The templates kept across sessions ([Render] projectile_store; PhantomRingHost.projectiles next to the DLL): the
		// first throw of each item in a session was the simple shape, its model learned only from it. What is kept is no MGSV
		// asset -- the guest's content hashes of the meshes and textures (the same in every session), the material id and each
		// part's matrix in the projectile's frame; the meshes themselves still come from the guest when MGSV draws them (right
		// after the release, or in Snake's hand before it), and until they are on the GPU the ball is drawn as before.
		constexpr std::uint32_t kStoreMagic = 0x4A505250;  // "PRPJ"
		constexpr std::uint32_t kStoreVersion = 1;
		bool                    g_loaded{ false };
		std::set<std::uint64_t> g_fromDisk;     // keys loaded from the store (logged the first time one is drawn)
		std::set<std::uint64_t> g_drawnFromDisk;
		std::uint64_t           g_storeWrites{ 0 };
		std::string             g_storeState{ "not read yet" };

		std::filesystem::path StorePath() { return std::filesystem::path(ModuleDir()) / L"PhantomRingHost.projectiles"; }

		template <class T>
		void Put(std::string& a_out, const T& a_v)
		{
			a_out.append(reinterpret_cast<const char*>(&a_v), sizeof(T));
		}
		template <class T>
		bool Get(const std::string& a_in, std::size_t& a_at, T& a_v)
		{
			if (a_at + sizeof(T) > a_in.size()) {
				return false;
			}
			std::memcpy(&a_v, a_in.data() + a_at, sizeof(T));
			a_at += sizeof(T);
			return true;
		}

		// Locked.
		void Save()
		{
			if (!Cfg().render.projectileStore) {
				return;
			}
			std::string out;
			Put(out, kStoreMagic);
			Put(out, kStoreVersion);
			Put(out, static_cast<std::uint32_t>(g_templates.size()));
			for (const auto& [key, t] : g_templates) {
				Put(out, key);
				Put(out, t.extent);
				Put(out, t.across);
				Put(out, static_cast<std::uint8_t>(t.refused));
				Put(out, t.at.x);
				Put(out, t.at.y);
				Put(out, t.at.z);
				Put(out, static_cast<std::uint32_t>(t.parts.size()));
				for (const auto& part : t.parts) {
					Put(out, part.record);
					Put(out, static_cast<std::uint32_t>(part.local.size()));
					out.append(reinterpret_cast<const char*>(part.local.data()), part.local.size() * sizeof(float));
				}
			}
			std::error_code ec;
			const auto      path = StorePath();
			auto            tmp = path;
			tmp += L".tmp";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				f.write(out.data(), static_cast<std::streamsize>(out.size()));
				if (!f) {
					g_storeState = std::format("could not write {}", tmp.string());
					Log::Warn("projectiles: {}", g_storeState);
					return;
				}
			}
			std::filesystem::rename(tmp, path, ec);
			if (ec) {
				g_storeState = std::format("could not replace {}: {}", path.string(), ec.message());
				Log::Warn("projectiles: {}", g_storeState);
				return;
			}
			++g_storeWrites;
			g_storeState = std::format("{} template(s) kept in {}", g_templates.size(), path.filename().string());
			Log::Info("projectiles: {}", g_storeState);
		}

		// Locked; once. A store that doesn't read back exactly (another version, cut short) is ignored: learned again.
		void Load()
		{
			if (g_loaded) {
				return;
			}
			g_loaded = true;
			if (!Cfg().render.projectileStore) {
				g_storeState = "off ([Render] projectile_store)";
				return;
			}
			std::ifstream f(StorePath(), std::ios::binary);
			if (!f) {
				g_storeState = "none yet (learned on the first throw of each item, then kept)";
				return;
			}
			const std::string in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			std::size_t       at = 0;
			std::uint32_t     magic = 0, version = 0, count = 0;
			std::map<std::uint64_t, projectile::Template> loaded;
			bool ok = Get(in, at, magic) && Get(in, at, version) && Get(in, at, count) && magic == kStoreMagic && version == kStoreVersion && count <= 256;
			for (std::uint32_t i = 0; ok && i < count; ++i) {
				std::uint64_t        key = 0;
				projectile::Template t;
				std::uint8_t         refused = 0;
				std::uint32_t        parts = 0;
				ok = Get(in, at, key) && Get(in, at, t.extent) && Get(in, at, t.across) && Get(in, at, refused) && Get(in, at, t.at.x) && Get(in, at, t.at.y) &&
				     Get(in, at, t.at.z) && Get(in, at, parts) && parts >= 1 && parts <= projectile::kMaxParts;
				t.refused = refused != 0;
				// Review 0.7.0: a hand-edited or damaged store (a flipped bit inside the floats keeps the size right) is ignored whole:
				// sizes in range, every number finite, and at least one matrix per part (0 used to emit a draw reading past the
				// matrices).
				const float limit = IsThrow(key) ? projectile::kMaxThrownAcross : projectile::kMaxRoundAcross;
				ok = ok && std::isfinite(t.extent) && t.extent >= 0.0f && t.extent <= projectile::kMaxExtent && std::isfinite(t.across) &&
				     (t.across < 0.0f || t.refused || t.across <= limit) && Finite(t.at);
				for (std::uint32_t p = 0; ok && p < parts; ++p) {
					projectile::Part part;
					std::uint32_t    floats = 0;
					ok = Get(in, at, part.record) && Get(in, at, floats) && floats >= 12 && floats % 12 == 0 && floats <= 12 * 256 &&
					     at + floats * sizeof(float) <= in.size();
					if (ok) {
						part.local.resize(floats);
						std::memcpy(part.local.data(), in.data() + at, floats * sizeof(float));
						at += floats * sizeof(float);
						ok = std::ranges::all_of(part.local, [](float v) { return std::isfinite(v); });
						t.parts.push_back(std::move(part));
					}
				}
				if (ok) {
					loaded[key] = std::move(t);
				}
			}
			if (!ok || at != in.size()) {
				g_storeState = std::format("{} unreadable ({} bytes): ignored, learned again", StorePath().filename().string(), in.size());
				Log::Warn("projectiles: {}", g_storeState);
				return;
			}
			for (auto& [key, t] : loaded) {
				g_fromDisk.insert(key);
				Log::Info("projectiles: {} known from an earlier session: {} part(s), first mesh {:016x}{}", KeyName(key), t.parts.size(), t.parts.front().record.mesh,
					t.refused ? " (refused: too big)" : t.across >= 0.0f ? std::format(", {:.2f} m across", t.across) : std::string());
				g_templates[key] = std::move(t);
			}
			g_storeState = std::format("{} template(s) read from {}", loaded.size(), StorePath().filename().string());
		}


		// Locked. All its meshes uploaded; measures its real size the first time (a mis-learned gun or arm is refused here).
		bool Ready(std::uint64_t a_key, projectile::Template& a_t)
		{
			if (a_t.parts.empty() || a_t.refused) {
				return false;
			}
			std::vector<projectile::Box> boxes;
			bool                         anyBox = false;
			for (const auto& p : a_t.parts) {
				const auto mesh = RenderAssets::FindMesh(p.record.mesh);
				if (!mesh) {
					return false;  // not uploaded (yet), or forgotten after the guest re-attached: the ball until it is sent again
				}
				projectile::Box b;
				b.valid = mesh->hasBounds;
				std::copy_n(mesh->boundsMin, 3, b.min);
				std::copy_n(mesh->boundsMax, 3, b.max);
				anyBox = anyBox || b.valid;
				boxes.push_back(b);
			}
			if (a_t.across < 0.0f) {
				a_t.across = projectile::Across(a_t, boxes);
				const float limit = IsThrow(a_key) ? projectile::kMaxThrownAcross : projectile::kMaxRoundAcross;
				if (!(a_t.across <= limit)) {
					a_t.refused = true;
					++g_rejected;
					Log::Warn("projectiles: the mesh learned for {} is {:.2f} m across (a projectile is at most {:.1f}): not one -- the ball stays until another "
					          "release learns one",
						KeyName(a_key), a_t.across, limit);
					Save();
					return false;
				}
				Log::Info("projectiles: {}'s mesh measures {:.2f} m across{}", KeyName(a_key), a_t.across, anyBox ? "" : " (no mesh box: part origins only)");
				Save();
			}
			return true;
		}

		// Locked. A confirmed template for a_key: the first good one stays; other meshes take over only when they keep coming.
		void Commit(std::uint64_t a_key, projectile::Template a_t)
		{
			const auto it = g_templates.find(a_key);
			if (it == g_templates.end() || it->second.refused) {
				++g_learned;
				Log::Info("projectiles: {} drawn with MGSV's mesh from now on: {} part(s), first mesh {:016x}, origins {:.2f} m apart", KeyName(a_key), a_t.parts.size(),
					a_t.parts.front().record.mesh, a_t.extent);
				g_templates[a_key] = std::move(a_t);
				g_challengers.erase(a_key);
				g_fromDisk.erase(a_key);
				Save();
				return;
			}
			auto signature = projectile::Signature(a_t);
			if (signature == projectile::Signature(it->second)) {
				++g_kept;
				g_challengers.erase(a_key);
				return;
			}
			auto& c = g_challengers[a_key];
			if (c.signature != signature) {
				c.signature = std::move(signature);
				c.count = 0;
			}
			if (++c.count < kReplaceAfter) {
				Log::Info("projectiles: {} showed other meshes this time ({} part(s), first {:016x}): the template learned first stays unless the next agrees",
					KeyName(a_key), a_t.parts.size(), a_t.parts.front().record.mesh);
				return;
			}
			++g_replaced;
			Log::Warn("projectiles: {} replaced: {} releases in a row showed other meshes ({} part(s), first {:016x})", KeyName(a_key), c.count, a_t.parts.size(),
				a_t.parts.front().record.mesh);
			it->second = std::move(a_t);
			g_challengers.erase(a_key);
			g_fromDisk.erase(a_key);
			Save();
		}

		// Locked, projectile_meshes on: this list's tagged draws for the newest unsettled release.
		void LearnFrom(const RenderSnapshot& a_s)
		{
			std::vector<projectile::Pending> pending;
			for (const auto& r : g_pending) {
				pending.push_back(r.pending);
			}
			const int at = projectile::PickRelease(pending, static_cast<double>(::GetTickCount64()), kLearnWithinMs);
			if (at < 0) {
				++g_unclaimed;
				return;
			}
			auto& r = g_pending[static_cast<std::size_t>(at)];
			auto  t = projectile::Learn(a_s.draws, a_s.matrices, r.dirMgsv);
			if (t.parts.empty()) {
				if (++r.tries >= kMaxTries) {
					r.pending.learned = true;
					++g_rejected;
					Log::Warn("projectiles: the guest's projectile draws for {} were not taken (more than {} parts or spread wider than {:.1f} m)", KeyName(r.pending.key),
						projectile::kMaxParts, projectile::kMaxExtent);
				}
				return;
			}
			if (!r.candidate) {
				r.candidate = std::move(t);
				r.candidateFrame = a_s.guestFrame;
				return;
			}
			if (a_s.guestFrame < r.candidateFrame + 3) {
				return;  // the same MGSV list, or one blended right next to it: too little motion to judge
			}
			if (projectile::Confirms(*r.candidate, r.candidateFrame, t, a_s.guestFrame, r.dirMgsv)) {
				r.pending.learned = true;
				Commit(r.pending.key, std::move(*r.candidate));  // its first list: later ones have it turned further by gravity
				return;
			}
			// It did not fly like the projectile (gear of Snake's, a prop down range) -- or the projectile showed up only now.
			r.candidate = std::move(t);
			r.candidateFrame = a_s.guestFrame;
			if (++r.tries >= kMaxTries) {
				r.pending.learned = true;
				++g_unconfirmed;
				Log::Warn("projectiles: the guest's projectile draws for {} never flew like it (the same meshes moving away along the release): not taken",
					KeyName(r.pending.key));
			}
		}
	}

	void NoteRelease(std::uint64_t a_key, const Vec3& a_erDir)
	{
		if (!Finite(a_erDir) || Length(a_erDir) < 1e-3f) {
			return;
		}
		const double now = static_cast<double>(::GetTickCount64());
		std::scoped_lock lock(g_lock);
		std::erase_if(g_pending, [&](const Release& a_r) { return now - a_r.pending.atMs > 3000.0; });
		if (g_pending.size() >= 16) {
			g_pending.erase(g_pending.begin());
		}
		Load();
		Release r;
		r.pending.key = a_key;
		r.pending.atMs = now;
		r.dirMgsv = Puppet::CurrentAnchor().DirToMgsv(Normalize(a_erDir));
		g_pending.push_back(std::move(r));
		++g_releases;
	}

	void Set(Source a_source, std::vector<Flying> a_flying)
	{
		std::scoped_lock lock(g_lock);
		// Poses of copies no longer flown (blasted, lost) go.
		std::erase_if(g_poses, [&](const auto& a_p) {
			return (a_p.first >> 32) - 1 == std::uint64_t(a_source) &&
			       std::ranges::none_of(a_flying, [&](const Flying& a_f) { return KeyOf(a_source, a_f.id) == a_p.first; });
		});
		g_flying[static_cast<std::size_t>(a_source)] = std::move(a_flying);
	}

	bool Drawn(Source a_source, std::uint32_t a_id, std::uint64_t a_erFrame)
	{
		std::scoped_lock lock(g_lock);
		return g_placedFrame + 1 >= a_erFrame && g_placedNow.contains(KeyOf(a_source, a_id));
	}

	bool Apply(RenderSnapshot& a_s)
	{
		const auto& rc = Cfg().render;
		const bool  on = rc.projectileMeshes;
		const bool  tagged = std::ranges::any_of(a_s.draws, [](const proto::DrawRecord& a_d) { return a_d.owner == proto::kOwnerProjectile; });
		std::scoped_lock lock(g_lock);
		Load();
		if (tagged) {
			++g_listsWithTags;
			if (on) {
				LearnFrom(a_s);
				// Never where MGSV has it: its grenade flies over MGSV's airport.
				std::erase_if(a_s.draws, [](const proto::DrawRecord& a_d) { return a_d.owner == proto::kOwnerProjectile; });
			} else {
				// Off: what the guest tagged is Snake's (near him) or out, as before v13 -- nothing taken from him, nothing added.
				const auto mine = [&] { return std::ranges::count_if(a_s.draws, [](const proto::DrawRecord& a_d) { return a_d.owner == proto::kOwnerPlayer; }); };
				const auto before = mine();
				projectile::HandBack(a_s.draws, a_s.matrices, rc.drawPlayer);
				g_handedBack += static_cast<std::uint64_t>(mine() - before);
			}
		}
		g_placedNow.clear();
		g_placedFrame = a_s.erFrame;
		if (on) {
			for (std::size_t s = 0; s < g_flying.size(); ++s) {
				const auto source = static_cast<Source>(s);
				for (const auto& f : g_flying[s]) {
					auto& pose = g_poses[KeyOf(source, f.id)];
					projectile::Advance(pose, f.vel, f.age, source == Source::kThrow, rc.projectileSpin);
					const auto it = g_templates.find(f.key);
					if (it == g_templates.end() || !Ready(f.key, it->second) || !Finite(f.pos)) {
						continue;
					}
					projectile::Place(it->second, projectile::ToListPoint(a_s.anchor, f.pos), projectile::ToListDir(a_s.anchor, pose.heading), pose.spin, a_s.draws,
						a_s.matrices);
					g_placedNow.insert(KeyOf(source, f.id));
					++g_placed;
					if (g_fromDisk.contains(f.key) && g_drawnFromDisk.insert(f.key).second) {
						Log::Info("projectiles: {} drawn with the mesh kept from an earlier session ({:.2f} s after its release: no learning needed)", KeyName(f.key),
							f.age);
					}
				}
			}
		}
		return !a_s.draws.empty();
	}

	void OnPuppetStop()
	{
		std::scoped_lock lock(g_lock);
		for (auto& l : g_flying) {
			l.clear();
		}
		g_pending.clear();
		g_poses.clear();
		g_placedNow.clear();
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		std::string out = std::format("projectile meshes {}: {} releases, {} lists with tagged draws, {} templates learned ({} kept; {} agreeing learns, {} replaced), "
		                              "{} refused, {} never flew, {} unclaimed, {} handed back to Snake (off); {} copies drawn with a mesh | store: {} ({} writes)",
			Cfg().render.projectileMeshes ? "on" : "off", g_releases, g_listsWithTags, g_learned, g_templates.size(), g_kept, g_replaced, g_rejected, g_unconfirmed,
			g_unclaimed, g_handedBack, g_placed, g_storeState, g_storeWrites);
		for (auto& [key, t] : g_templates) {
			const bool ready = Ready(key, t);
			out += std::format("\n {}{}: {} part(s), first mesh {:016x}, {}, {}", KeyName(key), g_fromDisk.contains(key) ? " (kept from an earlier session)" : "",
				t.parts.size(), t.parts.front().record.mesh,
				t.across >= 0.0f ? std::format("{:.2f} m across", t.across) : std::string("size not measured yet"),
				t.refused ? "REFUSED (too big)" : ready ? "mesh ready" : "mesh not on the GPU");
		}
		return out;
	}
}
