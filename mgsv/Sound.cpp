#include "Sound.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Logic.h"
#include "State.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <sstream>

namespace phantom::guest::sound
{
	namespace
	{
		using SetPositionFn = int (*)(std::uint64_t a_object, const float* a_position, std::uint32_t a_listener);
		using SetVolumeFn = int (*)(std::uint64_t a_object, float a_gain);
		SetPositionFn g_setPosition{ nullptr };
		SetVolumeFn   g_setVolume{ nullptr };
		using PostEventFn = std::uint32_t (*)(std::uint32_t a_event, std::uint64_t a_object, std::uint32_t a_flags, void* a_callback, void* a_cookie,
			std::uint32_t a_externals, void* a_externalSources, std::uint32_t a_playingId);
		PostEventFn g_postEvent{ nullptr };

		// Diagnostics (`!sound posts`): the last events posted, with their object.
		struct Post
		{
			std::atomic<std::uint64_t> ms{ 0 };
			std::atomic<std::uint64_t> object{ 0 };
			std::atomic<std::uint32_t> event{ 0 };
		};
		std::array<Post, 256>      g_posts;
		std::atomic<std::uint32_t> g_postNext{ 0 };

		// Events whose object gets a gain the moment they are posted (D-Walker's movement sounds are posted on fresh objects
		// that are never positioned: `!sound posts` while it walks). Up to 8, set from the ini and the console.
		struct EventGain
		{
			std::atomic<std::uint32_t> event{ 0 };
			std::atomic<float>         gain{ 1.0f };
			std::atomic<int>           mode{ 0 };  // bit0 dry volume, bit1 attenuation scaling, bit2 clear aux sends
		};
		using ScaleFn = int (*)(std::uint64_t, float);
		using AuxFn = int (*)(std::uint64_t, const void*, std::uint32_t);
		ScaleFn g_setScale{ nullptr };
		AuxFn   g_setAux{ nullptr };
		std::array<EventGain, 8>   g_eventGains;
		std::atomic<std::uint64_t> g_eventHits{ 0 };

		std::uint32_t PostEventDetour(std::uint32_t a_event, std::uint64_t a_object, std::uint32_t a_flags, void* a_callback, void* a_cookie,
			std::uint32_t a_externals, void* a_externalSources, std::uint32_t a_playingId)
		{
			auto& p = g_posts[g_postNext.fetch_add(1, std::memory_order_relaxed) & 255];
			p.event.store(a_event, std::memory_order_relaxed);
			p.object.store(a_object, std::memory_order_relaxed);
			p.ms.store(::GetTickCount64(), std::memory_order_release);
			// Before the event: Wwise may read an object's dry level / sends when it creates the voice (applied after, live: no
			// effect at all).
			for (const auto& e : g_eventGains) {
				if (e.event.load(std::memory_order_relaxed) == a_event && a_event) {
					const int   mode = e.mode.load(std::memory_order_relaxed);
					const float gain = e.gain.load(std::memory_order_relaxed);
					if (mode & 1) {
						g_setVolume(a_object, gain);
					}
					if ((mode & 2) && g_setScale && gain > 0.0f) {
						g_setScale(a_object, gain);
					}
					if ((mode & 4) && g_setAux) {
						g_setAux(a_object, nullptr, 0);
					}
					g_eventHits.fetch_add(1, std::memory_order_relaxed);
					break;
				}
			}
			const auto id = g_postEvent(a_event, a_object, a_flags, a_callback, a_cookie, a_externals, a_externalSources, a_playingId);
			return id;
		}
		bool          g_installed{ false };

		// Where every positioned Wwise game object was last put (Fox world: Wwise z is -Fox z, 0x140337302). Fox makes a
		// new game object for most one-shot sounds (each D-Walker footstep: live, ids counting up by thousands a minute),
		// positions it once and posts the sound, so an object is judged at its first position, in the detour itself.
		struct Slot
		{
			std::atomic<std::uint64_t> id{ 0 };
			std::atomic<std::uint64_t> ms{ 0 };
			std::atomic<float>         x{ 0 }, y{ 0 }, z{ 0 };
			std::atomic<bool>          walker{ false };  // given WalkerVolume
		};
		constexpr std::size_t      kSlots = 2048;
		std::array<Slot, kSlots>   g_slots;
		std::atomic<std::uint64_t> g_positions{ 0 }, g_dropped{ 0 }, g_tagged{ 0 }, g_failed{ 0 };

		// What the detour compares against, refreshed from MGSV's Lua thread every Tick (~30 Hz).
		std::atomic<bool>  g_armed{ false };  // walker deployed, not ridden, apart from Snake
		std::atomic<float> g_wx{ 0 }, g_wy{ 0 }, g_wz{ 0 }, g_sx{ 0 }, g_sz{ 0 }, g_gain{ 1.0f };

		std::size_t Hash(std::uint64_t a_id) { return static_cast<std::size_t>((a_id * 0x9E3779B97F4A7C15ull) >> 53) & (kSlots - 1); }

		Slot* Remember(std::uint64_t a_id, float a_x, float a_y, float a_z)
		{
			const auto  now = ::GetTickCount64();
			std::size_t i = Hash(a_id);
			for (int probe = 0; probe < 16; ++probe, i = (i + 1) & (kSlots - 1)) {
				auto&         s = g_slots[i];
				std::uint64_t have = s.id.load(std::memory_order_acquire);
				// Ours, empty, or an object nobody moved for 10 s (one-shot objects die within a second or two): take it.
				if (have != a_id && have != 0 && now - s.ms.load(std::memory_order_relaxed) < 10000) {
					continue;
				}
				if (have != a_id) {
					if (!s.id.compare_exchange_strong(have, a_id, std::memory_order_acq_rel)) {
						continue;
					}
					s.walker.store(false, std::memory_order_relaxed);
				}
				s.x.store(a_x, std::memory_order_relaxed);
				s.y.store(a_y, std::memory_order_relaxed);
				s.z.store(a_z, std::memory_order_relaxed);
				s.ms.store(now, std::memory_order_release);
				g_positions.fetch_add(1, std::memory_order_relaxed);
				return &s;
			}
			g_dropped.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}

		// D-Walker's: near the walker (its feet, legs, body: within 2.8 m and up to 4.5 m above its root), and clearly nearer
		// to it than to Snake (his own sounds are where he is; the check is off while the two are within 3 m).
		bool NearWalker(float a_x, float a_y, float a_z)
		{
			const float wx = g_wx.load(std::memory_order_relaxed), wz = g_wz.load(std::memory_order_relaxed);
			const float dw = std::hypot(a_x - wx, a_z - wz);
			if (dw > 2.8f) {
				return false;
			}
			const float ds = std::hypot(a_x - g_sx.load(std::memory_order_relaxed), a_z - g_sz.load(std::memory_order_relaxed));
			const float dy = a_y - g_wy.load(std::memory_order_relaxed);
			return ds > dw + 1.5f && dy > -1.0f && dy < 4.5f;
		}

		int SetPositionDetour(std::uint64_t a_object, const float* a_position, std::uint32_t a_listener)
		{
			const int r = g_setPosition(a_object, a_position, a_listener);
			if (a_object && a_position) {
				const float x = a_position[0], y = a_position[1], z = -a_position[2];
				auto*       s = Remember(a_object, x, y, z);
				// Queued right behind its position: Wwise applies it to the object's voices whether the sound was posted
				// before or after.
				if (s && g_armed.load(std::memory_order_relaxed) && !s->walker.load(std::memory_order_relaxed) && NearWalker(x, y, z)) {
					s->walker.store(true, std::memory_order_relaxed);
					if (g_setVolume(a_object, g_gain.load(std::memory_order_relaxed)) == 1) {  // AK_Success
						g_tagged.fetch_add(1, std::memory_order_relaxed);
					} else {
						g_failed.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			return r;
		}
	}

	bool Install()
	{
		if (std::fabs(Cfg().walkerVolume - 1.0f) < 0.001f) {
			Log::Info("sound: [Audio] WalkerVolume = 1 -- D-Walker keeps MGSV's volume");
			return false;
		}
		if (!Matches(addr::kAkSetPosition, "AK::SoundEngine::SetPosition") || !Matches(addr::kAkSetOutputBusVolume, "AK::SoundEngine::SetGameObjectOutputBusVolume")) {
			Log::Warn("sound: Wwise functions differ -- D-Walker keeps MGSV's volume");
			return false;
		}
		if (!Hooks::Create("AK SetPosition", reinterpret_cast<void*>(addr::kAkSetPosition.va), &SetPositionDetour, &g_setPosition)) {
			return false;
		}
		g_setVolume = reinterpret_cast<SetVolumeFn>(addr::kAkSetOutputBusVolume.va);
		if (Matches(addr::kAkSetScalingFactor, "AK SetAttenuationScalingFactor")) {
			g_setScale = reinterpret_cast<ScaleFn>(addr::kAkSetScalingFactor.va);
		}
		if (Matches(addr::kAkSetAuxSends, "AK SetGameObjectAuxSendValues")) {
			g_setAux = reinterpret_cast<AuxFn>(addr::kAkSetAuxSends.va);
		}
		if (Matches(addr::kAkPostEvent, "AK::SoundEngine::PostEvent")) {
			Hooks::Create("AK PostEvent", reinterpret_cast<void*>(addr::kAkPostEvent.va), &PostEventDetour, &g_postEvent);
		}
		g_installed = true;
		Log::Info("sound: D-Walker's Wwise objects -> output bus volume {:.2f} ({:.1f} dB)", Cfg().walkerVolume, 20.0f * std::log10(std::max(1e-4f, Cfg().walkerVolume)));
		return true;
	}

	void Tick(bool a_ready)
	{
		if (!g_installed) {
			return;
		}
		const auto lua = state::Lua();
		const auto fr = state::Frame();
		const bool deployed = a_ready && lua.InGame() && fr.valid && lua.buddyId != 0xFFFF && lua.buddyPosValid;
		const bool riding = lua.ridden != 0xFFFF || (lua.flags & state::kLVehicle);
		if (!deployed || riding || (lua.buddyPos - fr.root).LengthXZ() < 3.0f) {
			g_armed.store(false, std::memory_order_relaxed);
			return;
		}
		g_wx.store(lua.buddyPos.x, std::memory_order_relaxed);
		g_wy.store(lua.buddyPos.y, std::memory_order_relaxed);
		g_wz.store(lua.buddyPos.z, std::memory_order_relaxed);
		g_sx.store(fr.root.x, std::memory_order_relaxed);
		g_sz.store(fr.root.z, std::memory_order_relaxed);
		g_gain.store(std::clamp(Cfg().walkerVolume, 0.0f, 4.0f), std::memory_order_relaxed);
		g_armed.store(true, std::memory_order_release);
	}

	std::string Command(const std::string& a_args)
	{
		std::istringstream in(a_args);
		std::string        verb;
		in >> verb;
		if (verb == "volume") {
			float v = -1.0f;
			if (!(in >> v) || v < 0.0f || v > 4.0f) {
				return "usage: !sound volume <linear gain 0..4>";
			}
			MutableCfg().walkerVolume = v;
			return std::format("sound: D-Walker volume {:.2f} ({:.1f} dB) for its next sounds", v, 20.0f * std::log10(std::max(1e-4f, v)));
		}
		if (!g_installed) {
			return "sound: off (WalkerVolume = 1, or the Wwise functions differ: see the log)";
		}
		const auto now = ::GetTickCount64();
		if (verb == "event") {
			// !sound event <id> <gain>: every post of this event gets the gain on its object (gain 1 removes it)
			std::string idText;
			float       g = -1.0f;
			if (!(in >> idText >> g) || g < 0.0f || g > 4.0f) {
				return "usage: !sound event <event id, 0x..> <gain>";
			}
			const auto event = static_cast<std::uint32_t>(std::strtoul(idText.c_str(), nullptr, 0));
			int        mode = 1;
			in >> mode;
			for (auto& e : g_eventGains) {
				if (e.event.load() == event || e.event.load() == 0) {
					e.gain.store(g);
					e.mode.store(mode);
					e.event.store(std::fabs(g - 1.0f) < 1e-4f ? 0 : event);
					return std::format("sound: event {:#010x} -> gain {:.2f}", event, g);
				}
			}
			return "sound: event table full";
		}
		if (verb == "posts") {
			// !sound posts [ms]: events posted in the last ms (default 1000): event id, object, where that object was last put
			std::uint64_t ms = 1000;
			in >> ms;
			const auto  lua = state::Lua();
			const auto  fr = state::Frame();
			std::string out;
			int         n = 0;
			for (const auto& p : g_posts) {
				const auto at = p.ms.load(std::memory_order_acquire);
				if (!at || now - at > ms || n >= 60) {
					continue;
				}
				++n;
				const auto object = p.object.load();
				std::string where = "never positioned";
				for (const auto& s : g_slots) {
					if (s.id.load() == object) {
						const logic::Vec3 pos{ s.x.load(), s.y.load(), s.z.load() };
						where = std::format("at {:.1f},{:.1f},{:.1f} w {:.1f} s {:.1f}{}", pos.x, pos.y, pos.z, (pos - lua.buddyPos).LengthXZ(), (pos - fr.root).LengthXZ(),
							s.walker.load() ? " *" : "");
						break;
					}
				}
				out += std::format("{:5} ms ago event {:#010x} object {} {}\n", now - at, p.event.load(), object, where);
			}
			return out.empty() ? "no posts" : out;
		}
		if (verb == "all-volume" || verb == "all-scale") {
			// !sound all-volume <gain> | all-scale <factor>: every object positioned in the last 10 s (diagnostics: which
			// control reaches what you hear)
			float v = -1.0f;
			if (!(in >> v) || v < 0.0f || v > 100.0f || (verb == "all-scale" && v <= 0.0f)) {
				return "usage: !sound all-volume <gain> | all-scale <factor > 0>";
			}
			using ScaleFn = int (*)(std::uint64_t, float);
			const auto scale = Matches(addr::kAkSetScalingFactor, "AK SetAttenuationScalingFactor") ? reinterpret_cast<ScaleFn>(addr::kAkSetScalingFactor.va) : nullptr;
			if (verb == "all-scale" && !scale) {
				return "scaling factor function differs";
			}
			int n = 0;
			for (const auto& s : g_slots) {
				const auto id = s.id.load();
				if (id && now - s.ms.load() < 10000) {
					(verb == "all-scale" ? scale : g_setVolume)(id, v);
					++n;
				}
			}
			return std::format("sound: {} {:.2f} on {} objects", verb, v, n);
		}
		if (verb == "set") {
			// !sound set <object id> <gain>: one Wwise game object's output bus volume (finding which object is which)
			std::uint64_t id = 0;
			float         g = -1.0f;
			if (!(in >> id >> g) || !id || g < 0.0f || g > 4.0f) {
				return "usage: !sound set <AkGameObjectID> <gain 0..4>";
			}
			return std::format("sound: object {} -> {:.2f}: AKRESULT {}", id, g, g_setVolume(id, g));
		}
		if (verb == "all") {
			// !sound all [ms]: every object positioned in the last ms (default 1000), with its distance to the walker and Snake
			std::uint64_t ms = 1000;
			in >> ms;
			const auto  lua = state::Lua();
			const auto  fr = state::Frame();
			std::string out;
			int         n = 0;
			for (const auto& s : g_slots) {
				const auto id = s.id.load();
				if (!id || now - s.ms.load() > ms || n++ >= 60) {
					continue;
				}
				const logic::Vec3 at{ s.x.load(), s.y.load(), s.z.load() };
				out += std::format("{} {:.2f},{:.2f},{:.2f} w {:.1f} s {:.1f}{}\n", id, at.x, at.y, at.z, (at - lua.buddyPos).LengthXZ(), (at - fr.root).LengthXZ(),
					s.walker.load() ? " *" : "");
			}
			return out.empty() ? "nothing positioned" : out;
		}
		const auto lua = state::Lua();
		const auto fr = state::Frame();
		std::string out = std::format("sound: {} positions ({} dropped), D-Walker volume {:.2f} ({}), {} of its objects turned down ({} failed), {} event posts turned down",
			g_positions.load(), g_dropped.load(), Cfg().walkerVolume, g_armed.load() ? "armed" : "not armed: no walker, ridden or next to Snake", g_tagged.load(),
			g_failed.load(), g_eventHits.load());
		if (lua.buddyPosValid && fr.valid) {
			out += "\nobjects within 5 m of the walker, positioned in the last 2 s:";
			int n = 0;
			for (const auto& s : g_slots) {
				const auto id = s.id.load();
				if (!id || now - s.ms.load() > 2000) {
					continue;
				}
				const logic::Vec3 at{ s.x.load(), s.y.load(), s.z.load() };
				const float       dw = (at - lua.buddyPos).LengthXZ();
				if (dw < 5.0f && n++ < 24) {
					out += std::format("\n  {} at {:.2f},{:.2f},{:.2f}: {:.1f} m from it, {:.1f} m from Snake{}", id, at.x, at.y, at.z, dw, (at - fr.root).LengthXZ(),
						s.walker.load() ? " (turned down)" : "");
				}
			}
		}
		return out;
	}
}
