#include "Skeleton.h"

#include "ActorProxy.h"

#include "Addresses.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Logic.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <vector>
#include <cmath>
#include <format>
#include <string>
#include <cstring>
#include <mutex>

namespace phantom::guest::skeleton
{
	namespace
	{
		using PublishFn = void (*)(void*, void*);
		PublishFn g_original{ nullptr };

		// The player's target, resolved on the game thread, consumed on whichever thread publishes skins.
		std::atomic<std::uintptr_t> g_driver{ 0 };
		std::atomic<std::uintptr_t> g_model{ 0 };
		std::atomic<std::uint64_t>  g_frame{ 0 };
		std::uintptr_t              g_lastOwner{ 0 };
		std::uint64_t               g_lastResolve{ 0 };

		std::atomic<std::uint64_t> g_published{ 0 };
		std::mutex                 g_poseLock;
		proto::PoseBlock           g_pose{};  // last published, for BoneWorld
		ULONGLONG                  g_poseTick{ 0 };
		bool                       g_loggedLayout{ false };

		template <class T>
		bool Read(std::uintptr_t a_at, T& a_out)
		{
			const auto v = SafeRead<T>(reinterpret_cast<const void*>(a_at));
			if (v) {
				a_out = *v;
			}
			return v.has_value();
		}

		// ---- the buddy's skin (D-Horse, D-Dog, Quiet: their GameObjects answer no GetPosition; live 2026-10-13, TppHorse2
		// returned nothing) -------------------------------------------------------------------------------------------
		// SkinPublish runs for every skinned model with its (driver, binding); the binding's first 64 bytes are the model's
		// root matrix (the horse's: translation = where it stands, live: model 0x..980 +0xD0 followed it). The buddy's pair is
		// learned from where it must be (a hint: Snake's root while he rides it, where boot called it, beside Snake after a
		// whistle): the nearest other skin with a full skeleton (53+ bones, not Snake's 178) within the hint's radius. After that
		// every publication of that pair is the buddy's position and heading; a pair not published for 3 s is forgotten.
		constexpr std::uint32_t kSnakeBones = 178;
		// A buddy's full skeleton: D-Horse 69 bones (live); D-Walker's skins are rigid parts (it answers GetPosition anyway).
		constexpr std::uint32_t kMinBuddyBones = 53;
		std::atomic<std::uint32_t> g_rejectedBones{ 0 };
		struct M16
		{
			float v[16];
		};
		std::mutex              g_buddyLock;
		struct BuddyHint
		{
			float         pos[3]{};
			float         radius{ 0 };
			std::uint64_t untilMs{ 0 };
			bool          horizontal{ true };
		} g_hint;
		std::uintptr_t g_buddyDriver{ 0 };
		std::uintptr_t g_buddyBinding{ 0 };
		std::uint32_t  g_buddyBones{ 0 };
		std::uint32_t  g_buddyBonesLearned{ 0 };  // the count of the last one learned: a re-learn wants the same kind
		float          g_buddyRoot[16]{};
		std::uint64_t  g_buddyTick{ 0 };
		std::uint64_t  g_buddyLearnedAt{ 0 };
		std::uint32_t  g_buddyLearns{ 0 };
		std::uintptr_t g_candDriver{ 0 }, g_candBinding{ 0 };
		float          g_candDist{ 0 };
		std::uint32_t  g_candBones{ 0 };
		std::uint64_t  g_candTick{ 0 };
		std::atomic<bool> g_hinting{ false };

		std::uint32_t BonesOf(std::uintptr_t a_driver)
		{
			std::uintptr_t skeleton = 0;
			std::uint16_t  count = 0;
			std::int16_t   zero16 = -1;
			if (!Read(a_driver + 0x10, skeleton) || !skeleton || !Read(skeleton + 0x48, count) || !Read(skeleton + 0x4A, zero16) || zero16 != 0) {
				return 0;
			}
			return count;
		}

		// `!skins` (dev): every skinned model published for a while, with its root and bone count.
		struct Surveyed
		{
			std::uintptr_t driver, binding;
			std::uint32_t  bones;
			float          pos[3];
			std::uint32_t  seen;
		};
		std::mutex                g_surveyLock;
		std::vector<Surveyed>     g_survey;
		std::atomic<std::uint64_t> g_surveyUntil{ 0 };

		void SurveySkin(std::uintptr_t a_driver, std::uintptr_t a_binding, std::uint64_t a_now)
		{
			if (a_now > g_surveyUntil.load(std::memory_order_relaxed)) {
				return;
			}
			M16 mm;
			if (!Read(a_binding, mm)) {
				return;
			}
			std::scoped_lock lock(g_surveyLock);
			for (auto& e : g_survey) {
				if (e.driver == a_driver && e.binding == a_binding) {
					e.pos[0] = mm.v[12];
					e.pos[1] = mm.v[13];
					e.pos[2] = mm.v[14];
					++e.seen;
					return;
				}
			}
			if (g_survey.size() < 512) {
				g_survey.push_back({ a_driver, a_binding, BonesOf(a_driver), { mm.v[12], mm.v[13], mm.v[14] }, 1 });
			}
		}

		void ObserveBuddySkin(std::uintptr_t a_driver, std::uintptr_t a_binding)
		{
			const auto now = ::GetTickCount64();
			SurveySkin(a_driver, a_binding, now);
			if (a_driver == g_buddyDriver && a_binding == g_buddyBinding) {
				M16 mm;
				if (!Read(a_binding, mm) || !logic::MatrixTranslation(mm.v).Finite()) {
					return;
				}
				const float* m = mm.v;
				std::scoped_lock lock(g_buddyLock);
				if (a_driver == g_buddyDriver) {
					std::memcpy(g_buddyRoot, m, sizeof(g_buddyRoot));
					g_buddyTick = now;
				}
				return;
			}
			if (!g_hinting.load(std::memory_order_relaxed)) {
				return;
			}
			M16 mm;
			if (!Read(a_binding, mm)) {
				return;
			}
			const float* m = mm.v;
			BuddyHint h;
			{
				std::scoped_lock lock(g_buddyLock);
				h = g_hint;
			}
			if (now > h.untilMs) {
				g_hinting.store(false, std::memory_order_relaxed);
				return;
			}
			const float dx = m[12] - h.pos[0], dy = m[13] - h.pos[1], dz = m[14] - h.pos[2];
			const float d = h.horizontal ? std::sqrt(dx * dx + dz * dz) : std::sqrt(dx * dx + dy * dy + dz * dz);
			if (!std::isfinite(d) || d > h.radius || std::fabs(dy) > 3.0f) {
				return;
			}
			// Snake's own other skins (head, arm, gear: 64 bones and fewer) stand exactly on his root (live: one of 64 bones was
			// learned as the horse once): never the buddy. A horse can stand 0.4 m from it (live, dispatched from the iDroid).
			if (const auto snake = g_model.load(std::memory_order_acquire)) {
				M16 sm;
				if (Read(snake + addr::kModelBinding, sm)) {
					const float sx = m[12] - sm.v[12], sy = m[13] - sm.v[13], sz = m[14] - sm.v[14];
					if (sx * sx + sy * sy + sz * sz < 0.05f * 0.05f) {
						return;
					}
				}
			}
			const auto bones = BonesOf(a_driver);
			if (bones < kMinBuddyBones || bones > proto::kMaxPoseBones || bones == kSnakeBones || (g_buddyBonesLearned && bones != g_buddyBonesLearned)) {
				g_rejectedBones.store(bones, std::memory_order_relaxed);  // `!buddy`: what stood in the hint's window but didn't count
				return;
			}
			if (const float at[3] = { m[12], m[13], m[14] }; proxy::NearAnyProxy(at, 1.0f)) {
				return;  // an ActorProxy soldier ([Proxies], docs/GUEST.md 20), never the buddy
			}
			std::scoped_lock lock(g_buddyLock);
			// The nearest within the hint's window; taken once the same pair stayed the nearest for 10 publications (~1/6 s).
			if (g_candDriver == a_driver && g_candBinding == a_binding) {
				g_candDist = d;
				if (++g_candTick >= 10) {
					g_buddyDriver = a_driver;
					g_buddyBinding = a_binding;
					g_buddyBones = bones;
					g_buddyBonesLearned = bones;
					std::memcpy(g_buddyRoot, m, sizeof(g_buddyRoot));
					g_buddyTick = g_buddyLearnedAt = now;
					++g_buddyLearns;
					g_candDriver = g_candBinding = 0;
					g_hint.untilMs = 0;
					g_hinting.store(false, std::memory_order_relaxed);
					Log::Info("skeleton: buddy skin learned: driver {:#x} binding {:#x} ({} bones) at {:.2f},{:.2f},{:.2f}, {:.2f} m from the hint",
						a_driver, a_binding, bones, m[12], m[13], m[14], d);
				}
			} else if (!g_candDriver || d < g_candDist - 0.05f) {
				g_candDriver = a_driver;
				g_candBinding = a_binding;
				g_candDist = d;
				g_candBones = bones;
				g_candTick = 1;
			}
		}


		bool Resolve(std::uintptr_t a_owner, std::uintptr_t& a_driver, std::uintptr_t& a_model)
		{
			std::uintptr_t character = 0, component = 0, holder = 0, vt = 0;
			if (!Read(a_owner + addr::kOwnerCharacter, character) || !character || !Read(character, vt) || vt != addr::kCharacterVtable) {
				return false;
			}
			if (!Read(character + 0x10, component) || !component || !Read(component + 0x10, holder) || !holder) {
				return false;
			}
			if (!Read(holder + 0x68, a_model) || !a_model || !Read(a_model, vt) || vt != addr::kModelVtable) {
				return false;
			}
			if (!Read(holder + 0x70, a_driver) || !a_driver || !Read(a_driver, vt) || vt != addr::kDriverVtable) {
				return false;
			}
			return true;
		}

		void Publish(std::uintptr_t a_driver, std::uintptr_t a_model, std::uintptr_t a_binding)
		{
			const auto skeleton = *reinterpret_cast<const std::uintptr_t*>(a_driver + 0x10);
			if (!skeleton) {
				return;
			}
			const auto* sk = reinterpret_cast<const std::uint8_t*>(skeleton);
			std::uint16_t count;
			std::int16_t  zero16;
			std::uint32_t zero32, qo, po;
			std::memcpy(&count, sk + 0x48, 2);
			std::memcpy(&zero16, sk + 0x4A, 2);
			std::memcpy(&zero32, sk + 0x60, 4);
			std::memcpy(&qo, sk + 0x58, 4);
			std::memcpy(&po, sk + 0x5C, 4);
			// MGS5VR's layout checks (controller_rig.cpp apply()).
			if (count < 53 || count > proto::kMaxPoseBones || zero16 != 0 || zero32 != 0 || qo < 0x90 || po < qo + count * 16u || po > 0x10000) {
				if (!g_loggedLayout) {
					g_loggedLayout = true;
					Log::Warn("skeleton: pose buffer layout unexpected (count {} qo {:#x} po {:#x}) -- not published", count, qo, po);
				}
				return;
			}
			static proto::PoseBlock pose;  // only one publisher at a time (the player's skin): g_poseLock below
			std::scoped_lock        lock(g_poseLock);
			pose.boneCount = count;
			pose.frame = g_frame.load(std::memory_order_relaxed);
			std::memcpy(pose.root, reinterpret_cast<const void*>(a_binding), sizeof(pose.root));
			for (std::uint32_t i = 0; i < count; ++i) {
				std::memcpy(pose.bones[i].rot, sk + qo + i * 16u, 16);
				std::memcpy(pose.bones[i].pos, sk + po + i * 16u, 16);
			}
			// Parents from the model's table (stride 0x20). MGS5VR reads all `count` entries the same way.
			const auto parents = *reinterpret_cast<const std::uintptr_t*>(a_model + addr::kModelParents);
			bool       parentsOk = parents != 0;
			for (std::uint32_t i = 0; i < count && parentsOk; ++i) {
				std::int32_t p;
				std::memcpy(&p, reinterpret_cast<const void*>(parents + i * 0x20u), 4);
				parentsOk = p >= -1 && p < static_cast<std::int32_t>(i);
				pose.parent[i] = static_cast<std::int16_t>(p);
			}
			if (!parentsOk) {
				for (std::uint32_t i = 0; i < count; ++i) {
					pose.parent[i] = -2;  // unknown: the host draws points instead of bones
				}
			}
			link::PublishPose(pose);
			g_pose = pose;
			g_poseTick = ::GetTickCount64();
			if (g_published.fetch_add(1) == 0) {
				Log::Info("skeleton: first pose published ({} bones, parents {})", count, parentsOk ? "ok" : "unknown");
			}
		}

		void Detour(void* a_driver, void* a_binding)
		{
			g_original(a_driver, a_binding);
			const auto driver = g_driver.load(std::memory_order_acquire);
			const auto model = g_model.load(std::memory_order_acquire);
			if (!driver || reinterpret_cast<std::uintptr_t>(a_driver) != driver || reinterpret_cast<std::uintptr_t>(a_binding) != model + addr::kModelBinding) {
				if (reinterpret_cast<std::uintptr_t>(a_driver) != driver) {
					try {
						ObserveBuddySkin(reinterpret_cast<std::uintptr_t>(a_driver), reinterpret_cast<std::uintptr_t>(a_binding));
					} catch (...) {
					}
				}
				return;
			}
			try {
				Publish(driver, model, reinterpret_cast<std::uintptr_t>(a_binding));
			} catch (...) {
			}
		}
	}

	bool Install()
	{
		if (!Matches(addr::kSkinPublish, "SkinPublish")) {
			Log::Error("skeleton: SkinPublish differs -- no pose");
			return false;
		}
		return Hooks::Create("SkinPublish", reinterpret_cast<void*>(addr::kSkinPublish.va), &Detour, &g_original);
	}

	void ObserveOwner(std::uintptr_t a_owner, std::uint64_t a_frame)
	{
		g_frame.store(a_frame, std::memory_order_relaxed);
		// The chain only changes on loads/model swaps: re-resolve on a new owner and every ~1 s.
		if (a_owner == g_lastOwner && a_frame - g_lastResolve < 60) {
			return;
		}
		g_lastOwner = a_owner;
		g_lastResolve = a_frame;
		std::uintptr_t driver = 0, model = 0;
		if (!Resolve(a_owner, driver, model)) {
			driver = model = 0;
		}
		if (driver != g_driver.load() || model != g_model.load()) {
			Log::Info("skeleton: player skin target driver {:#x} model {:#x}", driver, model);
		}
		g_model.store(model, std::memory_order_release);
		g_driver.store(driver, std::memory_order_release);
	}

	bool BoneWorld(int a_bone, float a_out[3])
	{
		std::scoped_lock lock(g_poseLock);
		if (a_bone < 0 || static_cast<std::uint32_t>(a_bone) >= g_pose.boneCount || ::GetTickCount64() - g_poseTick > 200) {
			return false;
		}
		const auto& b = g_pose.bones[a_bone];
		const auto  p = logic::TransformPoint(g_pose.root, { b.pos[0], b.pos[1], b.pos[2] });
		a_out[0] = p.x;
		a_out[1] = p.y;
		a_out[2] = p.z;
		return p.Finite();
	}

	std::uint64_t PublishedFrames() { return g_published.load(); }

	void HintBuddy(const logic::Vec3& a_at, float a_radius, std::uint64_t a_forMs, bool a_horizontal)
	{
		std::scoped_lock lock(g_buddyLock);
		if (g_buddyDriver && ::GetTickCount64() - g_buddyTick < 3000) {
			return;  // tracked and fresh: nothing to learn
		}
		if (g_buddyDriver) {
			Log::Info("skeleton: buddy skin {:#x} not published for {} ms -- learning it again", g_buddyDriver, ::GetTickCount64() - g_buddyTick);
			g_buddyDriver = g_buddyBinding = 0;
		}
		const bool was = g_hint.untilMs > ::GetTickCount64();
		g_hint.pos[0] = a_at.x;
		g_hint.pos[1] = a_at.y;
		g_hint.pos[2] = a_at.z;
		g_hint.radius = a_radius;
		g_hint.horizontal = a_horizontal;
		g_hint.untilMs = ::GetTickCount64() + a_forMs;
		if (!was) {
			g_candDriver = g_candBinding = 0;
		}
		g_hinting.store(true, std::memory_order_relaxed);
	}

	void ForgetBuddy(const char* a_why)
	{
		std::scoped_lock lock(g_buddyLock);
		if (g_buddyDriver) {
			Log::Info("skeleton: buddy skin {:#x} forgotten ({})", g_buddyDriver, a_why);
		}
		g_buddyDriver = g_buddyBinding = 0;
		g_buddyBonesLearned = 0;
		g_hint.untilMs = 0;
		g_candDriver = g_candBinding = 0;
	}

	bool BuddyPose(logic::Vec3& a_pos, float& a_yaw, std::uint64_t a_maxAgeMs)
	{
		std::scoped_lock lock(g_buddyLock);
		if (!g_buddyDriver || ::GetTickCount64() - g_buddyTick > a_maxAgeMs) {
			return false;
		}
		a_pos = logic::MatrixTranslation(g_buddyRoot);
		a_yaw = logic::MatrixYaw(g_buddyRoot);
		return a_pos.Finite() && std::isfinite(a_yaw);
	}

	void StartSurvey(std::uint64_t a_ms)
	{
		std::scoped_lock lock(g_surveyLock);
		g_survey.clear();
		g_surveyUntil.store(::GetTickCount64() + a_ms);
	}

	std::string DescribeSurvey(const logic::Vec3& a_at, float a_radius)
	{
		std::scoped_lock lock(g_surveyLock);
		std::vector<std::pair<float, const Surveyed*>> hits;
		for (const auto& e : g_survey) {
			const float dx = e.pos[0] - a_at.x, dz = e.pos[2] - a_at.z;
			const float d = std::sqrt(dx * dx + dz * dz);
			if (d <= a_radius) {
				hits.emplace_back(d, &e);
			}
		}
		std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		std::string out = std::format("{} skins published ({} within {:.0f} m of {:.1f},{:.1f},{:.1f})\n", g_survey.size(), hits.size(), a_radius, a_at.x, a_at.y,
			a_at.z);
		for (const auto& [d, e] : hits) {
			out += std::format("  driver {:#x} binding {:#x} {} bones at {:.2f},{:.2f},{:.2f} ({:.1f} m, {} times)\n", e->driver, e->binding, e->bones, e->pos[0],
				e->pos[1], e->pos[2], d, e->seen);
		}
		return out;
	}

	std::string DescribeBuddy()
	{
		std::scoped_lock lock(g_buddyLock);
		const auto now = ::GetTickCount64();
		if (!g_buddyDriver) {
			return std::format("buddy skin: not known ({} learned so far; {}; last skin in a hint's window not taken: {} bones)", g_buddyLearns,
				g_hint.untilMs > now ? std::format("learning near {:.1f},{:.1f},{:.1f} r {:.1f}", g_hint.pos[0], g_hint.pos[1], g_hint.pos[2], g_hint.radius)
				                     : std::string("no hint"),
				g_rejectedBones.load());
		}
		const auto p = logic::MatrixTranslation(g_buddyRoot);
		return std::format("buddy skin: driver {:#x} ({} bones) at {:.2f},{:.2f},{:.2f} yaw {:.1f}, {} ms ago, learned {} s ago ({} learns)", g_buddyDriver,
			g_buddyBones, p.x, p.y, p.z, logic::MatrixYaw(g_buddyRoot) * 57.2958f, now - g_buddyTick, (now - g_buddyLearnedAt) / 1000, g_buddyLearns);
	}
}
