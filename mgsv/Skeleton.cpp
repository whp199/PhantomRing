#include "Skeleton.h"

#include "Addresses.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Logic.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
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
}
