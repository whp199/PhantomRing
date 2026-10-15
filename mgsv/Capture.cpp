#include "Capture.h"

#include "ActorProxy.h"

#include "CaptureAssets.h"
#include "CaptureMesh.h"
#include "CaptureRing.h"
#include "Config.h"
#include "FultonAnchor.h"
#include "D3D11Hooks.h"
#include "Hud.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "ProjectileTag.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace phantom::guest::capture
{
	namespace
	{
		// ---- frame inputs (game thread) -------------------------------------------------------------
		std::mutex g_frameLock;
		struct FrameIn
		{
			std::uint64_t frame{ 0 };
			float         root[3]{};
			float         yaw{ 0 };
			float         cam[3]{};
			bool          buddy{ false };
			float         buddyPos[3]{};
		} g_in;

		// The last camera frames (game thread): which root a draw belongs to is found through the camera
		// position the draw was recorded with (cVSScene.m_eyepos == that frame's camera-setter position).
		struct FrameRec
		{
			std::uint64_t frame{ 0 };
			float         root[3]{};
			float         yaw{ 0 };
			float         cam[3]{};
		};
		constexpr std::size_t kHistory = 64;
		FrameRec              g_history[kHistory];
		std::size_t           g_historyNext{ 0 };

		std::optional<FrameRec> FrameForEye(const float a_eye[3])
		{
			std::scoped_lock lock(g_frameLock);
			std::optional<FrameRec> best;
			for (const auto& r : g_history) {
				if (!r.frame) {
					continue;
				}
				const float dx = r.cam[0] - a_eye[0], dy = r.cam[1] - a_eye[1], dz = r.cam[2] - a_eye[2];
				if (dx * dx + dy * dy + dz * dz < 1e-4f && (!best || r.frame > best->frame)) {
					best = r;
				}
			}
			return best;
		}

		// ---- publishing (DRAW CONTRACT) ---------------------------------------------------------------
		constexpr std::uint32_t kRoles = proto::kDrawTextures;

		// One of Snake's (or the buddy's) G-buffer draws as recorded, kept until the next Present.
		struct Captured
		{
			MeshSource       src;
			std::uint64_t    srcHash{ 0 };
			ID3D11Texture2D* tex[kRoles]{};
			std::uint64_t    frame{ 0 };
			float            anchor[3]{};
			float            yaw{ 0 };
			float            world[12]{};
			float            bones[32][12]{};
			std::uint32_t    boneCount{ 0 };  // 0: rigid
			std::uint32_t    owner{ proto::kOwnerPlayer };
			std::uint32_t    material{ 0 };
			float            params[8]{};
		};
		std::mutex            g_accLock;
		std::vector<Captured> g_acc;
		std::atomic<float>    g_projFovY{ 0.0f };

		// Present thread only.
		struct MeshEntry
		{
			std::uint64_t key{ 0 };
			std::uint32_t indexCount{ 0 };
			std::uint32_t bones{ 0 };
			bool          weights{ false };
			bool          failed{ false };
			std::uint32_t sentGeneration{ ~0u };
			std::vector<std::uint8_t> payload;  // kept for a resend after a host re-attach
			// The game's buffers this entry was built from and which copy of each (assets::Serial): see Stale().
			ID3D11Resource* res[d3d::kVbSlots + 1]{};
			std::uint64_t   serial[d3d::kVbSlots + 1]{};
		};
		struct TexEntry
		{
			std::uint64_t key{ 0 };
			bool          failed{ false };
			std::uint32_t sentGeneration{ ~0u };
			std::uint64_t serial{ 0 };  // which copy of the game's texture the key is for (assets::Serial); 0 = none yet
			std::shared_ptr<const assets::TextureBytes> bytes;
		};
		std::unordered_map<std::uint64_t, MeshEntry>   g_meshes;    // by MeshSource::Hash (session-local identity)
		std::unordered_map<ID3D11Texture2D*, TexEntry> g_textures;  // by the game's texture (AddRef'd by assets)
		std::atomic<std::uint64_t>                      g_published{ 0 }, g_lastDraws{ 0 }, g_skipped{ 0 }, g_meshesSent{ 0 }, g_texturesSent{ 0 }, g_proxyDrops{ 0 };
		std::atomic<std::uint64_t>                      g_restarted{ 0 }, g_pruned{ 0 };  // pointer-keyed entries dropped as stale (Stale())
		std::uint64_t                                   g_presentCount{ 0 };
		std::vector<std::uint8_t>                       g_list;

		// Affine 3x4 (rows: x' = m0 x + m1 y + m2 z + m3): out = a * b.
		void Mul34(const float* a, const float* b, float* out)
		{
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 4; ++c) {
					out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + (c == 3 ? a[r * 4 + 3] : 0.0f);
				}
			}
		}

		bool IsGBuffer(const d3d::RtInfo& a_rt)
		{
			return a_rt.count == 3 && a_rt.formats[0] == DXGI_FORMAT_B8G8R8A8_UNORM && a_rt.formats[1] == DXGI_FORMAT_B8G8R8A8_UNORM &&
			       a_rt.formats[2] == DXGI_FORMAT_B8G8R8A8_UNORM;
		}

		// Recording thread (any of MGSV's 24): keep the draws that are Snake's or his buddy's.
		void CaptureDraw(d3d::ContextState& a_s, const d3d::DrawArgs& a_a)
		{
			if (!IsGBuffer(a_s.rt) || !a_s.vsInfo || !a_s.psInfo || !a_a.indexed || a_a.instances != 1) {
				return;
			}
			const auto* objectCb = a_s.vsInfo->CBufferNamed("cVSObject");
			const auto* sceneCb = a_s.vsInfo->CBufferNamed("cVSScene");
			if (!objectCb || !sceneCb) {
				return;
			}
			float object[52]{};
			float eye[4]{};
			if (!d3d::ReadConstants(a_s, a_s.vsCb[objectCb->slot], 0, object, sizeof(object)) || !d3d::ReadConstants(a_s, a_s.vsCb[sceneCb->slot], 320, eye, 12)) {
				return;
			}
			// m_projection @64: element [1][1] = 1/tan(fovY/2) (same place row- or column-major).
			if (float proj[16]{}; d3d::ReadConstants(a_s, a_s.vsCb[sceneCb->slot], 64, proj, sizeof(proj)) && proj[5] > 0.1f && proj[5] < 50.0f) {
				g_projFovY.store(2.0f * std::atan(1.0f / proj[5]), std::memory_order_relaxed);
			}
			const float* world = object + 16;  // m_world @64: three rows
			FrameIn      in;
			{
				std::scoped_lock lock(g_frameLock);
				in = g_in;
			}
			const auto  rec = FrameForEye(eye);
			const float* root = rec ? rec->root : in.root;
			// Whose draw: the nearer of Snake's root and the buddy's position. The walker parks within 2.5 m of Snake
			// (it follows him), so "near Snake" alone tagged all of it as Snake's (no buddy fades, occluder test or
			// ground snap in Elden Ring); Snake's own draws sit on his root, the walker's on its own.
			const float dx = world[3] - root[0], dy = world[7] - root[1], dz = world[11] - root[2];
			const float ds = dx * dx + dy * dy + dz * dz;
			float       db = 1e30f;
			if (in.buddy) {
				const float bx = world[3] - in.buddyPos[0], by = world[7] - in.buddyPos[1], bz = world[11] - in.buddyPos[2];
				db = bx * bx + by * by + bz * bz;
			}
			std::uint32_t owner = ~0u;
			// (v13) The grenade / rocket Snake just released: a rigid draw where it must be by now (ProjectileTag.h), near him or not.
			const float origin[3] = { world[3], world[7], world[11] };
			if (!(object[32] > 0.0f) && projectile::Tag(rec ? rec->frame : in.frame, origin)) {
				owner = proto::kOwnerProjectile;
			} else if (ds < 2.5f * 2.5f && ds <= db) {
				owner = proto::kOwnerPlayer;
			} else if (db < 4.0f * 4.0f) {
				owner = proto::kOwnerBuddy;
			}
			if (owner == ~0u) {
				return;
			}
			// ActorProxy ([Proxies]): a pool soldier standing in for an Elden Ring enemy near Snake or the buddy is never theirs.
			// The Fulton anchor's sleeping soldier lies right beside Snake (FultonAnchor.cpp): never his either. A projectile tag too:
			// a proxy's rifle can sit on a rocket's line; a grenade passing a proxy only loses that frame's tag (the host needs two).
			if (proxy::ExcludesDraw(origin, root) || fulton::ExcludesDraw(origin, root)) {
				g_proxyDrops.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			Captured c;
			if (!DescribeMesh(a_s, a_a, c.src)) {
				g_skipped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			c.srcHash = c.src.Hash();
			c.frame = rec ? rec->frame : in.frame;
			std::copy_n(root, 3, c.anchor);
			c.yaw = rec ? rec->yaw : in.yaw;
			std::copy_n(world, 12, c.world);
			c.owner = owner;
			c.material = static_cast<std::uint32_t>(a_s.psInfo->hash);
			c.params[0] = object[32];  // m_useWeightCount.x
			const auto* bonesCb = a_s.vsInfo->CBufferNamed("VSBones");
			const bool  skinned = bonesCb && object[32] > 0.0f && a_s.vsInfo->Reads("BLENDINDICES");
			if (skinned && d3d::ReadConstants(a_s, a_s.vsCb[bonesCb->slot], 0, c.bones, sizeof(c.bones))) {
				c.boneCount = 32;
			}
			if (const auto* mat = a_s.psInfo->CBufferNamed("cPSMaterial")) {
				float m[8]{};
				if (d3d::ReadConstants(a_s, a_s.psCb[mat->slot], 0, m, sizeof(m))) {
					std::copy_n(m, 7, c.params + 1);
				}
			}
			// Textures by role from the pixel shader's own names (GUEST.md 15.2).
			unsigned extra = 3;
			for (unsigned i = 0; i < 8; ++i) {
				const auto* r = a_s.psInfo->TextureAt(i);
				if (!r || !a_s.psSrv[i]) {
					continue;
				}
				int role = -1;
				if (r->name == "g_tex_diffuse") {
					role = 0;
				} else if (r->name == "g_tex_normal") {
					role = 1;
				} else if (r->name == "g_tex_srm") {
					role = 2;
				} else if (r->name.starts_with("g_tex_") && r->name != "g_tex_mesh" && extra < kRoles) {
					role = static_cast<int>(extra++);
				}
				ID3D11Texture2D*     tex = nullptr;
				D3D11_TEXTURE2D_DESC d{};
				if (role >= 0 && d3d::SrvTexture(a_s.psSrv[i], &tex, &d)) {
					c.tex[role] = tex;
				}
			}
			std::scoped_lock lock(g_accLock);
			g_acc.push_back(std::move(c));
		}

		// Our caches below are keyed by MGSV's pointers, but our reference to a resource lasts only as long as its copy
		// (assets::Evict, a minute after its last use): after that MGSV may free it and create another one at the same
		// address. Before 2026-10-14 the keys were kept forever, so a texture made later at a freed address (a buddy
		// deployed later, a higher-resolution texture streamed in as the camera nears) went out as the OLD texture's key:
		// D-Dog's coat and D-Walker next to the camera in other maps' colours, the box black (docs/GUEST.md 15.3).
		using keys::Stale;  // the copy an entry was made from is no longer the current one (CaptureKeys.h, unit-tested)

		void RememberSources(MeshEntry& a_m, const MeshSource& a_src)
		{
			for (unsigned i = 0; i < d3d::kVbSlots; ++i) {
				a_m.res[i] = a_src.vb[i].buffer;
			}
			a_m.res[d3d::kVbSlots] = a_src.ib;
			for (unsigned i = 0; i <= d3d::kVbSlots; ++i) {
				a_m.serial[i] = a_m.res[i] ? assets::Serial(a_m.res[i], false) : 0;
			}
		}

		bool MeshStale(const MeshEntry& a_m, bool a_touch)
		{
			bool stale = false;
			for (unsigned i = 0; i <= d3d::kVbSlots; ++i) {
				if (a_m.res[i]) {
					stale |= Stale(a_m.serial[i], assets::Serial(a_m.res[i], a_touch));  // every one touched: a mesh in use keeps all its buffers
				}
			}
			return stale;
		}

		// Present thread: send what the host doesn't have yet. True when the mesh can be drawn this frame.
		MeshEntry* EnsureMesh(const Captured& a_c, std::uint32_t a_gen)
		{
			auto& m = g_meshes[a_c.srcHash];
			if (Cfg().captureRevalidate && keys::Revalidate(m, [](const MeshEntry& a_m) { return MeshStale(a_m, true); })) {
				g_restarted.fetch_add(1, std::memory_order_relaxed);
			}
			if (m.failed) {
				return nullptr;
			}
			if (m.payload.empty()) {
				std::shared_ptr<const std::vector<std::uint8_t>> vb[d3d::kVbSlots];
				bool                                             ready = true;
				for (unsigned i = 0; i < d3d::kVbSlots; ++i) {
					if (a_c.src.vb[i].buffer) {
						vb[i] = assets::Buffer(a_c.src.vb[i].buffer);
						ready &= vb[i] != nullptr;
						if (assets::Failed(a_c.src.vb[i].buffer)) {
							m.failed = true;
						}
					}
				}
				const auto ib = assets::Buffer(a_c.src.ib);
				if (assets::Failed(a_c.src.ib)) {
					m.failed = true;
				}
				RememberSources(m, a_c.src);  // the copies just asked for (queued, ready or failed): a failure is retried once they go
				if (m.failed || !ready || !ib) {
					return nullptr;
				}
				std::string why;
				auto        built = BuildMesh(a_c.src, vb, *ib, why);
				if (!built) {
					m.failed = true;
					Log::Warn("capture: mesh {:#018x} not built: {}", a_c.srcHash, why);
					return nullptr;
				}
				m.key = built->key;
				m.indexCount = built->indexCount;
				m.bones = built->bones;
				m.weights = built->weights;
				m.payload = std::move(built->payload);
				Log::Info("capture: mesh {:016x}: {} verts, {} indices, {} bones ({}) {}", m.key, built->vertexCount, m.indexCount, m.bones, a_c.owner == proto::kOwnerBuddy ? "buddy" : a_c.owner == proto::kOwnerProjectile ? "projectile" : "Snake",
					built->layout);
			}
			if (m.sentGeneration != a_gen) {
				std::uint32_t gen = 0;
				const auto    r = ring::Push(proto::kRenMesh, { { m.payload.data(), m.payload.size() } }, gen);
				if (r != ring::Result::kOk) {
					return nullptr;
				}
				m.sentGeneration = gen;
				g_meshesSent.fetch_add(1, std::memory_order_relaxed);
			}
			return &m;
		}

		std::uint64_t EnsureTexture(ID3D11Texture2D* a_tex, std::uint32_t a_gen, int& a_budget)
		{
			if (!a_tex) {
				return 0;
			}
			auto& t = g_textures[a_tex];
			// Another texture at this address now (or the same one again): copy and key it afresh. The touch keeps it in use.
			if (Cfg().captureRevalidate && keys::Revalidate(t, [a_tex](const TexEntry& a_t) { return Stale(a_t.serial, assets::Serial(a_tex, true)); })) {
				g_restarted.fetch_add(1, std::memory_order_relaxed);
			}
			if (t.failed) {
				return 0;
			}
			if (!t.bytes) {
				t.bytes = assets::Texture(a_tex);
				t.serial = assets::Serial(a_tex, false);
				if (!t.bytes) {
					t.failed = assets::Failed(a_tex);
					return 0;
				}
				const auto& d = t.bytes->desc;
				const std::uint32_t seed[5] = { static_cast<std::uint32_t>(d.Format), d.Width, d.Height, d.MipLevels, 1 };
				t.key = d3d::Hash64(t.bytes->bytes.data(), t.bytes->bytes.size(), d3d::Hash64(seed, sizeof(seed)));
				Log::Info("capture: texture {:016x} {}x{} {} mips {} ({} KB)", t.key, d.Width, d.Height, d3d::FormatName(d.Format), d.MipLevels, t.bytes->bytes.size() / 1024);
			}
			if (t.sentGeneration != a_gen) {
				if (a_budget <= 0) {
					return 0;  // big uploads spread over frames
				}
				const auto&      d = t.bytes->desc;
				proto::RenTexture hdr{ t.key, static_cast<std::uint32_t>(d.Format), d.Width, d.Height, d.MipLevels, 1, static_cast<std::uint32_t>(t.bytes->bytes.size()) };
				std::uint32_t    gen = 0;
				const auto       r = ring::Push(proto::kRenTexture, { { &hdr, sizeof(hdr) }, { t.bytes->bytes.data(), t.bytes->bytes.size() } }, gen);
				if (r != ring::Result::kOk) {
					if (r == ring::Result::kTooBig) {
						t.failed = true;
					}
					return 0;
				}
				--a_budget;
				t.sentGeneration = gen;
				g_texturesSent.fetch_add(1, std::memory_order_relaxed);
			}
			return t.key;
		}

		void Publish()
		{
			std::vector<Captured> acc;
			{
				std::scoped_lock lock(g_accLock);
				acc.swap(g_acc);
			}
			if (acc.empty()) {
				return;
			}
			// Draws of a newer game frame may already be recording: publish the oldest frame's, keep the rest.
			std::uint64_t oldest = ~0ull;
			for (const auto& c : acc) {
				oldest = std::min(oldest, c.frame);
			}
			std::vector<Captured> now, later;
			for (auto& c : acc) {
				(c.frame == oldest ? now : later).push_back(std::move(c));
			}
			if (!later.empty()) {
				std::scoped_lock lock(g_accLock);
				g_acc.insert(g_acc.begin(), std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
			}
			const auto gen = ring::Generation() ? ring::Generation() : link::AttachGeneration();
			int        texBudget = 2;
			std::vector<proto::DrawRecord> records;
			std::vector<float>             matrices;
			const auto&                    first = now.front();
			for (const auto& c : now) {
				auto* mesh = EnsureMesh(c, link::AttachGeneration());
				if (!mesh) {
					continue;
				}
				proto::DrawRecord r{};
				r.mesh = mesh->key;
				for (unsigned i = 0; i < kRoles; ++i) {
					r.textures[i] = EnsureTexture(c.tex[i], link::AttachGeneration(), texBudget);
				}
				r.indexCount = mesh->indexCount;
				r.startIndex = 0;
				r.baseVertex = 0;
				r.firstMatrix = static_cast<std::uint32_t>(matrices.size() / 12);
				r.owner = c.owner;
				r.material = c.material;
				std::copy_n(c.params, 8, r.params);
				const bool rigid = c.boneCount == 0 || mesh->bones == 0;
				const std::uint32_t n = rigid ? 1 : std::min<std::uint32_t>(std::max<std::uint32_t>(mesh->bones, 1), 32);
				for (std::uint32_t b = 0; b < n; ++b) {
					float m[12];
					if (rigid) {
						std::copy_n(c.world, 12, m);
					} else {
						Mul34(c.world, c.bones[b], m);  // bones are model space: world = m_world * bone
					}
					m[3] -= first.anchor[0];
					m[7] -= first.anchor[1];
					m[11] -= first.anchor[2];
					matrices.insert(matrices.end(), m, m + 12);
				}
				r.matrixCount = n;
				r.flags = rigid ? static_cast<std::uint32_t>(proto::kDrawRigid) : 0u;
				records.push_back(r);
			}
			(void)gen;
			if (records.empty()) {
				return;
			}
			proto::DrawList hdr{};
			hdr.frame = first.frame;
			hdr.drawCount = static_cast<std::uint32_t>(records.size());
			hdr.matrixCount = static_cast<std::uint32_t>(matrices.size() / 12);
			std::copy_n(first.anchor, 3, hdr.anchorPos);
			hdr.anchorYaw = first.yaw;
			g_list.resize(sizeof(hdr) + records.size() * sizeof(proto::DrawRecord) + matrices.size() * sizeof(float));
			std::memcpy(g_list.data(), &hdr, sizeof(hdr));
			std::memcpy(g_list.data() + sizeof(hdr), records.data(), records.size() * sizeof(proto::DrawRecord));
			std::memcpy(g_list.data() + sizeof(hdr) + records.size() * sizeof(proto::DrawRecord), matrices.data(), matrices.size() * sizeof(float));
			if (ring::PublishDraws(g_list.data(), g_list.size(), hdr.frame)) {
				g_published.fetch_add(1, std::memory_order_relaxed);
				g_lastDraws.store(records.size(), std::memory_order_relaxed);
			}
		}

		// ---- probe: a full description of the next N frames, written to PhantomRingCapture.txt -------
		std::atomic<int>  g_probeFrames{ 0 };  // frames still to describe
		std::atomic<bool> g_probing{ false };  // the current frame is being described
		std::mutex        g_probeLock;
		std::string       g_probeText;
		struct PassStats
		{
			d3d::RtInfo   rt;
			std::uint64_t draws{ 0 }, skinned{ 0 }, ui{ 0 };
			std::set<std::uint64_t> vs;
		};
		std::map<std::uint64_t, PassStats> g_passes;
		std::set<std::uint64_t>            g_described;  // shaders/layouts already described in this probe
		int                                g_detailed{ 0 };
		int                                g_uiLines{ 0 };
		std::uint64_t                      g_probeDraws{ 0 };

		std::string RtText(const d3d::RtInfo& a_rt)
		{
			std::string s = std::format("{}x{} [", a_rt.width, a_rt.height);
			for (unsigned i = 0; i < a_rt.count; ++i) {
				s += std::format("{}{}({})", i ? " " : "", d3d::FormatName(a_rt.formats[i]), a_rt.formats[i]);
			}
			s += std::format("] depth {}({})", d3d::FormatName(a_rt.depthFormat), a_rt.depthFormat);
			return s;
		}

		void ProbeTargets(d3d::ContextState& a_s)
		{
			if (!g_probing.load(std::memory_order_relaxed)) {
				return;
			}
			std::scoped_lock lock(g_probeLock);
			if (a_s.rt.count == 3) {
				g_probeText += std::format("  ctx#{}{} OMSetRenderTargets {}\n", a_s.index, a_s.deferred ? "d" : "i", RtText(a_s.rt));
			}
		}

		std::string CbDump(const d3d::ContextState& a_s, unsigned a_slot, const d3d::CbBind& a_b, std::uint32_t a_bytes)
		{
			std::vector<float> f(a_bytes / 4);
			if (!d3d::ReadConstants(a_s, a_b, 0, f.data(), a_bytes)) {
				return "      (contents not seen on this context)\n";
			}
			std::string out;
			for (std::size_t i = 0; i < f.size(); i += 4) {
				out += std::format("      [{:4}] {:10.4f} {:10.4f} {:10.4f} {:10.4f}\n", i * 4, f[i], f[i + 1], f[i + 2], f[i + 3]);
			}
			return out;
		}

		void ProbeDraw(d3d::ContextState& a_s, const d3d::DrawArgs& a_a)
		{
			std::scoped_lock lock(g_probeLock);
			++g_probeDraws;
			auto& p = g_passes[a_s.rt.signature];
			p.rt = a_s.rt;
			++p.draws;
			// Fox binds one input layout with every semantic; what the VS reads says whether it skins.
			const bool skinned = a_s.vsInfo && a_s.vsInfo->Reads("BLENDINDICES");
			p.skinned += skinned;
			if (a_s.vsInfo) {
				p.vs.insert(a_s.vsInfo->hash);
			}
			if (hud::InUiJob()) {  // MGSV's UI jobs (Hud.cpp): one line per draw, for the HUD layer
				++p.ui;
				if (g_uiLines++ < 400) {
					g_probeText += hud::ProbeLine(a_s, a_a);
				}
				return;
			}
			if (a_s.rt.count != 3) {  // the G-buffer (three BGRA8 targets + D32S8) only
				return;
			}
			std::string& t = g_probeText;
			FrameIn      in;
			{
				std::scoped_lock l2(g_frameLock);
				in = g_in;
			}
			// One summary line per G-buffer draw: where bone 0 / the object sits relative to Snake.
			float bone0[12]{}, object[52]{};
			const auto* bonesCb = a_s.vsInfo ? a_s.vsInfo->CBufferNamed("VSBones") : nullptr;
			const auto* objectCb = a_s.vsInfo ? a_s.vsInfo->CBufferNamed("cVSObject") : nullptr;
			const bool  haveBone = bonesCb && d3d::ReadConstants(a_s, a_s.vsCb[bonesCb->slot], 0, bone0, 48);
			const bool  haveObject = objectCb && d3d::ReadConstants(a_s, a_s.vsCb[objectCb->slot], 0, object, 208);
			std::string where;
			if (haveBone) {
				where += std::format(" bone0 T {:.2f},{:.2f},{:.2f}", bone0[3], bone0[7], bone0[11]);
			}
			if (haveObject) {
				where += std::format(" world T {:.2f},{:.2f},{:.2f} / {:.2f},{:.2f},{:.2f} weights {}", object[16 + 3], object[16 + 7], object[16 + 11], object[16 + 12],
					object[16 + 13], object[16 + 14], object[32]);
			}
			t += std::format("  G ctx#{} {} vs {:#018x} ps {:#018x} idx {} start {} base {} inst {} ib {} vb0 {}+{}{}\n", a_s.index, skinned ? "SKIN" : "rigid",
				a_s.vsInfo ? a_s.vsInfo->hash : 0, a_s.psInfo ? a_s.psInfo->hash : 0, a_a.indexCount, a_a.startIndex, a_a.baseVertex, a_a.instances,
				static_cast<void*>(a_s.ib), static_cast<void*>(a_s.vb[0].buffer), a_s.vb[0].offset, where);
			// Full details for what sits near Snake (his root, from the game thread) and a few others.
			const float dx = object[16 + 3] - in.root[0], dy = object[16 + 7] - in.root[1], dz = object[16 + 11] - in.root[2];
			const bool  nearSnake = haveObject && dx * dx + dy * dy + dz * dz < 16.0f;
			if (!nearSnake && ++g_detailed > 10) {
				return;
			}
			t += std::format("  DRAW ctx#{}{} {} {} idx {} start {} base {} inst {} topo {}\n", a_s.index, a_s.deferred ? "d" : "i", skinned ? "SKINNED" : "rigid",
				a_a.indexed ? "indexed" : "plain", a_a.indexCount, a_a.startIndex, a_a.baseVertex, a_a.instances, a_s.topology);
			if (a_s.layoutInfo) {
				t += std::format("    layout {:#018x}: {}\n", a_s.layoutInfo->hash, a_s.layoutInfo->Describe());
			}
			for (unsigned i = 0; i < d3d::kVbSlots; ++i) {
				if (a_s.vb[i].buffer) {
					D3D11_BUFFER_DESC d{};
					a_s.vb[i].buffer->GetDesc(&d);
					t += std::format("    vb{} {} stride {} offset {} size {} usage {} bind {:#x}\n", i, static_cast<void*>(a_s.vb[i].buffer), a_s.vb[i].stride, a_s.vb[i].offset,
						d.ByteWidth, static_cast<int>(d.Usage), d.BindFlags);
				}
			}
			if (a_s.ib) {
				D3D11_BUFFER_DESC d{};
				a_s.ib->GetDesc(&d);
				t += std::format("    ib {} {} offset {} size {} usage {}\n", static_cast<void*>(a_s.ib), d3d::FormatName(a_s.ibFormat), a_s.ibOffset, d.ByteWidth,
					static_cast<int>(d.Usage));
			}
			if (a_s.vsInfo && g_described.insert(a_s.vsInfo->hash).second) {
				t += "    " + a_s.vsInfo->Describe() + "\n";
			}
			if (a_s.psInfo && g_described.insert(a_s.psInfo->hash).second) {
				t += "    " + a_s.psInfo->Describe() + "\n";
			}
			if (haveObject) {
				t += "    cVSObject:\n" + CbDump(a_s, objectCb->slot, a_s.vsCb[objectCb->slot], 208);
			}
			if (bonesCb) {
				t += "    VSBones (first 8):\n" + CbDump(a_s, bonesCb->slot, a_s.vsCb[bonesCb->slot], 384);
			}
			for (unsigned i = 0; i < d3d::kPsSrvSlots; ++i) {
				ID3D11Texture2D*     tex = nullptr;
				D3D11_TEXTURE2D_DESC d{};
				if (a_s.psSrv[i] && d3d::SrvTexture(a_s.psSrv[i], &tex, &d)) {
					const auto* r = a_s.psInfo ? a_s.psInfo->TextureAt(i) : nullptr;
					t += std::format("    ps t{} {} tex {} {}x{} mips {} arr {} {}({})\n", i, r ? r->name : "?", static_cast<void*>(tex), d.Width, d.Height, d.MipLevels,
						d.ArraySize, d3d::FormatName(d.Format), static_cast<int>(d.Format));
				}
			}
		}

		void OnDraw(d3d::ContextState& a_s, const d3d::DrawArgs& a_a)
		{
			if (g_probing.load(std::memory_order_relaxed)) {
				ProbeDraw(a_s, a_a);
			}
			if (link::HostAlive()) {
				CaptureDraw(a_s, a_a);
			}
			hud::OnDraw(a_s, a_a);
		}

		void EndProbeFrame()
		{
			std::scoped_lock lock(g_probeLock);
			FrameIn in;
			{
				std::scoped_lock l2(g_frameLock);
				in = g_in;
			}
			std::string head = std::format("==== probe frame (present {}), game frame {} root {:.3f},{:.3f},{:.3f} yaw {:.3f} cam {:.3f},{:.3f},{:.3f} buddy {} {:.2f},{:.2f},{:.2f}\n",
				d3d::Presents(), in.frame, in.root[0], in.root[1], in.root[2], in.yaw, in.cam[0], in.cam[1], in.cam[2], in.buddy, in.buddyPos[0], in.buddyPos[1],
				in.buddyPos[2]);
			const auto st = d3d::Counters();
			head += std::format("  totals: draws {} (deferred {}) cbMaps {} cbUpdates {} executes {} finishes {} contexts {} layouts {} vs {} ps {}\n", st.draws, st.drawsDeferred,
				st.cbMaps, st.cbUpdates, st.executes, st.finishes, st.contexts, st.layouts, st.vertexShaders, st.pixelShaders);
			head += std::format("  this frame: {} draws\n", g_probeDraws);
			for (const auto& [sig, p] : g_passes) {
				head += std::format("  pass {:#018x}: {} draws ({} skinned, {} in UI jobs), {} vertex shaders, targets {}\n", sig, p.draws, p.skinned, p.ui, p.vs.size(),
					RtText(p.rt));
			}
			const auto path = ModuleDir() + L"PhantomRingCapture.txt";
			if (FILE* f = _wfopen(path.c_str(), L"ab")) {
				std::fwrite(head.data(), 1, head.size(), f);
				std::fwrite(g_probeText.data(), 1, g_probeText.size(), f);
				std::fclose(f);
			}
			Log::Info("capture: probe frame written ({} draws, {} passes, {} bytes)", g_probeDraws, g_passes.size(), head.size() + g_probeText.size());
			g_probeText.clear();
			g_passes.clear();
			g_detailed = 0;
			g_uiLines = 0;
			g_probeDraws = 0;
		}

		// Present thread, after an eviction: forget what was keyed by pointers whose copies are gone (and their bytes, kept for
		// resends), so nothing of theirs is sent again and the memory goes.
		void Prune()
		{
			const auto n = keys::Prune(g_textures, [](ID3D11Texture2D* a_tex, const TexEntry& a_t) { return Stale(a_t.serial, assets::Serial(a_tex, false)); }) +
			               keys::Prune(g_meshes, [](std::uint64_t, const MeshEntry& a_m) { return MeshStale(a_m, false); });
			g_pruned.fetch_add(n, std::memory_order_relaxed);
		}

		void OnPresent(ID3D11DeviceContext* a_imm)
		{
			assets::Pump(d3d::Device(), a_imm);
			if (++g_presentCount % 600 == 0) {
				assets::Evict(g_presentCount);
				if (Cfg().captureRevalidate) {
					Prune();
				}
			}
			hud::OnPresent(d3d::Device(), a_imm);
			if (link::HostAlive()) {
				Publish();
			} else {
				std::scoped_lock lock(g_accLock);
				g_acc.clear();
			}
			if (g_probing.exchange(false)) {
				EndProbeFrame();
			}
			if (g_probeFrames.load() > 0) {
				g_probeFrames.fetch_sub(1);
				std::scoped_lock lock(g_probeLock);
				g_described.clear();
				g_probing.store(true);
			}
		}
	}

	void InstallEarly()
	{
		d3d::Callbacks cb;
		cb.draw = &OnDraw;
		cb.present = &OnPresent;
		cb.setTargets = &ProbeTargets;
		if (!d3d::InstallEarly(cb)) {
			Log::Warn("capture: D3D11CreateDevice not hooked; relying on the game's device global");
		}
		if (Cfg().hud) {
			hud::InstallEarly();
		}
	}

	void Poll()
	{
		try {
			d3d::Poll();
		} catch (...) {
		}
	}

	std::string Command(std::string_view a_args)
	{
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (a_args.starts_with("hud")) {
			return hud::Command(a_args.substr(3));
		}
		if (a_args.starts_with("probe")) {
			int n = 1;
			if (a_args.size() > 5) {
				n = std::max(1, std::atoi(std::string(a_args.substr(5)).c_str()));
			}
			g_probeFrames.store(std::min(n, 10));
			return std::format("capture: describing the next {} frame(s) into PhantomRingCapture.txt (present hooked: {})", n, d3d::PresentHooked());
		}
		const auto st = d3d::Counters();
		return std::format("capture: lists published {} (last {} draws), meshes sent {} ({} known), textures sent {} ({} known), skipped {}, proxy draws dropped {}, stale entries restarted {} / pruned {}, ring pending {} KB; {}\n",
			g_published.load(), g_lastDraws.load(), g_meshesSent.load(), g_meshes.size(), g_texturesSent.load(), g_textures.size(), g_skipped.load(), g_proxyDrops.load(),
			g_restarted.load(), g_pruned.load(),
			ring::Pending() / 1024,
			assets::Describe()) +
		hud::Describe() + "\n" + projectile::Describe() + "\n" +
		std::format("capture: device {} present hooked {} presents {} draws {} (deferred {}) cbMaps {} cbUpdates {} executes {} finishes {} contexts {} layouts {} vs {} ps {}",
			static_cast<void*>(d3d::Device()), d3d::PresentHooked(), d3d::Presents(), st.draws, st.drawsDeferred, st.cbMaps, st.cbUpdates, st.executes, st.finishes,
			st.contexts, st.layouts, st.vertexShaders, st.pixelShaders);
	}

	void NoteFrame(std::uint64_t a_frame, const float a_root[3], float a_yaw, const float a_camPos[3])
	{
		std::scoped_lock lock(g_frameLock);
		g_in.frame = a_frame;
		std::copy_n(a_root, 3, g_in.root);
		g_in.yaw = a_yaw;
		std::copy_n(a_camPos, 3, g_in.cam);
		auto& r = g_history[g_historyNext++ % kHistory];
		r.frame = a_frame;
		std::copy_n(a_root, 3, r.root);
		r.yaw = a_yaw;
		std::copy_n(a_camPos, 3, r.cam);
	}

	void NoteBuddy(bool a_present, const float a_pos[3])
	{
		std::scoped_lock lock(g_frameLock);
		g_in.buddy = a_present;
		if (a_present) {
			std::copy_n(a_pos, 3, g_in.buddyPos);
		}
	}

	float ProjectionFovY() { return g_projFovY.load(std::memory_order_relaxed); }
}
