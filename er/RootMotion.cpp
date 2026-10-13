#include "RootMotion.h"

#include "Addresses.h"
#include "Link.h"
#include "Log.h"
#include "VptrSwap.h"

#include <atomic>
#include <format>

namespace phantom::er::RootMotion
{
	namespace
	{
		constexpr std::size_t kPlayerCtrlSlots = 22;  // RTTI vtable walk (re-backlog E1)
		constexpr std::size_t kModifySlot = 12;       // +0x60, the call at 0x3CC85C

		using ModifyFn = void (*)(std::uint8_t* a_ctrl, float* a_rm);

		VptrSwap<kPlayerCtrlSlots>  g_swap;
		ModifyFn                    g_original{ nullptr };
		std::atomic<std::uint8_t*>  g_ctrl{ nullptr };
		std::atomic<std::uint8_t*>  g_phys{ nullptr };
		std::atomic<std::uint64_t>  g_calls{ 0 };
		std::atomic<std::int64_t>   g_lastCall{ 0 };
		std::atomic_flag            g_lock = ATOMIC_FLAG_INIT;  // accumulator: task thread vs behaviour update
		Vec3                        g_accum;
		Vec3                        g_lastApplied;

		struct Guard
		{
			Guard()
			{
				while (g_lock.test_and_set(std::memory_order_acquire)) {
					YieldProcessor();
				}
			}
			~Guard() { g_lock.clear(std::memory_order_release); }
		};

		Vec3 Take()
		{
			Guard guard;
			const Vec3 d = g_accum;
			g_accum = {};
			g_lastApplied = d;
			return d;
		}

		void Detour(std::uint8_t* a_ctrl, float* a_rm)
		{
			g_original(a_ctrl, a_rm);
			if (a_ctrl != g_ctrl.load(std::memory_order_acquire)) {
				return;  // a PlayerCtrl we no longer drive (left on our vtable copy): plain game behaviour
			}
			g_calls.fetch_add(1, std::memory_order_relaxed);
			g_lastCall.store(Link::Qpc(), std::memory_order_relaxed);
			auto* phys = g_phys.load(std::memory_order_acquire);
			if (!phys || !a_rm) {
				return;
			}
			const Vec3  d = Take();
			const auto* q = reinterpret_cast<const float*>(phys + game::kPhysOrientation);
			// 0x467280 does worldDisp = rotate(q, rm.xyz); hand it conj(q) * our world displacement.
			const Quat  inverse{ -q[0], -q[1], -q[2], q[3] };
			const Vec3  local = Rotate(inverse, d);
			if (!Finite(local)) {
				return;
			}
			a_rm[0] = local.x;
			a_rm[1] = local.y;
			a_rm[2] = local.z;
		}
	}

	bool Install(const game::Player& a_player)
	{
		const auto& a = Addr();
		if (!a.HasAll(A::VtPlayerCtrl, A::ModifyRootMotion, A::RootMotionCallsite) || !a_player) {
			Log::Warn("root motion: PlayerCtrl vtable / ModifyRootMotion / its call site unverified -- kinematic motion");
			return false;
		}
		// Before the swap: the behaviour update may call the detour the moment the vptr changes.
		g_original = reinterpret_cast<ModifyFn>(a[A::ModifyRootMotion]);
		if (!g_swap.Install(a_player.ctrl, a[A::VtPlayerCtrl], kModifySlot, a[A::ModifyRootMotion], reinterpret_cast<void*>(&Detour))) {
			Log::Warn("root motion: the player's ChrCtrl is not a stock PlayerCtrl with slot 12 = ModifyRootMotion ({}) -- kinematic motion",
				game::RttiName(a_player.ctrl));
			return false;
		}
		Clear();
		g_lastCall.store(0, std::memory_order_relaxed);
		g_phys.store(a_player.phys, std::memory_order_release);
		g_ctrl.store(a_player.ctrl, std::memory_order_release);
		Log::Info("root motion: PlayerCtrl {} slot 12 -> PhantomRing (instance vtable copy)", static_cast<void*>(a_player.ctrl));
		return true;
	}

	void Remove(const game::Player& a_player)
	{
		auto* ctrl = g_ctrl.exchange(nullptr, std::memory_order_acq_rel);
		g_phys.store(nullptr, std::memory_order_release);
		Clear();
		if (ctrl && a_player && a_player.ctrl == ctrl && g_swap.Restore(ctrl)) {
			Log::Info("root motion: PlayerCtrl vtable restored after {} calls", g_calls.load());
		}
	}

	void Add(const Vec3& a_d)
	{
		if (!Finite(a_d)) {
			return;
		}
		Guard guard;
		g_accum += Vec3{ a_d.x, 0.0f, a_d.z };  // ER's gravity and ground decide the vertical
	}

	void Clear()
	{
		Guard guard;
		g_accum = {};
	}

	bool Alive(double a_withinMs)
	{
		const auto last = g_lastCall.load(std::memory_order_relaxed);
		return last != 0 && Link::QpcToMs(Link::Qpc() - last) < a_withinMs;
	}

	bool Installed() { return g_ctrl.load(std::memory_order_acquire) != nullptr; }

	const void* FakeVtable() { return g_swap.Fake(); }

	std::string Describe()
	{
		if (!Installed()) {
			return "root motion off";
		}
		const auto last = g_lastCall.load(std::memory_order_relaxed);
		Vec3       lastStep;
		{
			Guard guard;
			lastStep = g_lastApplied;
		}
		return std::format("root motion ON: {} hook calls, last {:.0f} ms ago, last step ({:.3f}, {:.3f})", g_calls.load(),
			last ? Link::QpcToMs(Link::Qpc() - last) : -1.0, lastStep.x, lastStep.z);
	}
}
