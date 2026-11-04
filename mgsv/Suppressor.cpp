#include "Suppressor.h"

#include "Addresses.h"
#include "Ammo.h"
#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Logic.h"
#include "SaveGuard.h"
#include "Vars.h"
#include "Verify.h"
#include "WindowHidden.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <optional>
#include <utility>

namespace phantom::guest::suppressor
{
	namespace
	{
		std::atomic<bool> g_ok{ false };     // the reader replica may be used (MGSV's bytes match)
		std::atomic<bool> g_setOk{ false };  // the component's setters verified
		// the frame thread's last logged reading of the weapon in hand (equip | slot << 16 | on << 24 | life << 32)
		std::uint64_t g_lastInHand{ ~0ull };
		// console requests for the Lua thread
		std::mutex  g_lock;
		int         g_qSlot{ -1 }, g_qLife{ 0 };
		bool        g_qOn{ true };
		std::string g_lastSet{ "none yet" };
		// the guns that have a suppressor this session (the loadout's, or seen working): slot -> equip id (0 = none), for the refill
		std::array<std::atomic<std::uint16_t>, 3> g_has{};
		// a refill asked by the frame thread (a supply drop's ammo picked up) or Ammo.cpp, run on the Lua thread: why ("" = none)
		std::string   g_refillWhy;
		std::uint64_t g_refills{ 0 };  // under g_lock
		std::atomic<std::uint64_t> g_pickups{ 0 };
		std::string   g_lastRefill{ "none yet" };
		// the frame thread's last stock counts of the three weapon slots (vars.ammoStockCounts 0/2/4) and when a rest refilled
		std::array<int, 3>         g_lastStock{ -1, -1, -1 };
		std::atomic<std::uint64_t> g_restAt{ 0 };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		// MGSV can't save now (Pump.cpp's rule for what a save would keep): [Saves] Block, the file guards and Steam's storage swapped.
		// Every suppressor write (the refill, the console) waits for it, as the boot's loadout does.
		bool SavesGuarded() { return Cfg().blockSaves && hidden::SavesBlocked() && saves::SteamGuarded(); }
	}

	bool Init()
	{
		const bool ok = Matches(addr::kSuppressorState, "suppressor reader") && Matches(addr::kFireSuppressorRead, "FireWeapon -> suppressor reader") &&
		                Matches(addr::kFireSuppressorWear, "FireWeapon suppressor wear");
		g_ok.store(ok);
		const bool setOk = ok && Matches(addr::kAmmoSetSuppressorLife, "ammo SetSuppressorLife") && Matches(addr::kAmmoSetUseSuppressor, "ammo SetUseSuppressor") &&
		                   Matches(addr::kAmmoListenerGunData, "ammo listener GunData") &&
		                   Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfSetSuppressorLife * 8) == addr::kAmmoSetSuppressorLife.va &&
		                   Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfSetUseSuppressor * 8) == addr::kAmmoSetUseSuppressor.va;
		g_setOk.store(setOk);
		if (!ok) {
			Log::Warn("suppressor: MGSV's reader or FireWeapon's use of it differs -- no kGuestSuppressorOn / kFireSuppressed, no refill");
		} else if (!setOk) {
			Log::Warn("suppressor: the ammo component's suppressor setters differ -- read only (no refill at a rest)");
		} else {
			Log::Info("suppressor: MGSV's reader, its wear in FireWeapon and the component's setters verified");
		}
		return ok;
	}

	bool Available() { return g_ok.load(); }
	bool SettersAvailable() { return g_setOk.load(); }

	Reading Read(std::uintptr_t a_component, std::uintptr_t a_state, std::uint32_t a_player)
	{
		Reading r;
		if (!g_ok.load(std::memory_order_relaxed) || !a_component || !a_state) {
			return r;
		}
		logic::SuppressorInputs in;
		const auto flags = Get<std::uint32_t>(a_state + 0x27C);
		const auto equip = Get<std::uint32_t>(a_state + 0x24C);
		const auto mode = Get<std::uint8_t>(a_state + 0x3D5);
		const auto slot = Get<std::uint8_t>(a_state + addr::kWeaponAmmoSlot);
		const auto traits = Get<std::uint8_t>(a_state + addr::kWeaponGunTraits);
		if (!flags || !equip || !mode || !slot || !traits) {
			return r;
		}
		in.gunFlags = *flags;
		in.equip = *equip;
		in.mode = *mode;
		in.gunTraits = *traits;
		r.slot = *slot;
		r.equip = static_cast<std::uint16_t>(*equip);
		// the component MGSV's reader walks to: [[[c+8]+0x138]+0xC8]+8
		const auto character = Get<std::uintptr_t>(a_component + 8);
		const auto impl = character && *character ? Get<std::uintptr_t>(*character + 0x138) : std::nullopt;
		const auto ctrl = impl && *impl ? Get<std::uintptr_t>(*impl + 0xC8) : std::nullopt;
		const auto comp = ctrl && *ctrl ? Get<std::uintptr_t>(*ctrl + 8) : std::nullopt;
		if (comp && *comp && Get<std::uintptr_t>(*comp) == addr::kAmmoVtable) {
			const auto per = Get<std::uint32_t>(*comp + 0xC);
			const auto types = Get<std::uintptr_t>(*comp + 0x40);
			const auto records = Get<std::uintptr_t>(*comp + 0x50);
			if (per && *per <= 16 && types && *types && records && *records) {
				const std::uintptr_t idx = std::uintptr_t(*per) * a_player + *slot;
				const auto           type = Get<std::uint32_t>(*types + idx * 4);
				const auto           life = Get<std::uint8_t>(*records + idx * 6 + addr::kAmmoRecordLife);
				const auto           fl = Get<std::uint8_t>(*records + idx * 6 + addr::kAmmoRecordFlags);
				if (type && life && fl) {
					in.slotType = *type;
					in.haveRecord = true;
					in.recordLife = *life;
					in.recordFlags = *fl;
				}
			}
		} else if ((in.gunFlags & 0x1C0) == 0x40) {
			return r;  // a firearm whose component isn't readable: unknown, not "off"
		}
		const auto m = logic::SuppressorOf(in);
		r.valid = true;
		r.firearm = m.firearm;
		r.on = m.on;
		r.life = m.life;
		return r;
	}

	Reading InHand(std::uintptr_t a_character, std::uint32_t a_player)
	{
		if (!g_ok.load(std::memory_order_relaxed) || !a_character || a_player >= 16) {
			return {};
		}
		const auto c = Get<std::uintptr_t>(a_character + addr::kCharacterWeaponComponent);
		if (!c || !*c || Get<std::uintptr_t>(*c) != addr::kWeaponComponentVtable || Get<std::uintptr_t>(*c + 8) != a_character) {
			return {};
		}
		const auto range = Get<std::uintptr_t>(*c + 0x38);
		const auto first = range && *range ? Get<std::uint32_t>(*range + 0x24) : std::nullopt;
		const auto states = Get<std::uintptr_t>(*c + 0x58);
		if (!first || !states || !*states || a_player < *first || a_player - *first >= 16) {
			return {};
		}
		return Read(*c, *states + std::uintptr_t(a_player - *first) * addr::kWeaponStateBytes, a_player);
	}

	std::string Set(std::uintptr_t a_component, std::uint32_t a_player, int a_slot, std::uint8_t a_life, bool a_on)
	{
		if (!g_setOk.load()) {
			return "the setters are unverified";
		}
		if (!a_component || Get<std::uintptr_t>(a_component) != addr::kAmmoVtable || a_slot < 0 || a_slot > 2) {
			return "no ammo component";
		}
		const auto per = Get<std::uint32_t>(a_component + 0xC);
		const auto types = Get<std::uintptr_t>(a_component + 0x40);
		const auto records = Get<std::uintptr_t>(a_component + 0x50);
		if (!per || *per < 3 || *per > 16 || !types || !*types || !records || !*records) {
			return "slot table unreadable";
		}
		const std::uintptr_t idx = std::uintptr_t(*per) * a_player + std::uintptr_t(a_slot);
		const auto           type = Get<std::uint32_t>(*types + idx * 4);
		if (!type || (*type & 0x1F) < 1 || (*type & 0x1F) > 8) {
			return std::format("slot {}: not a gun slot (type {:#x})", a_slot, type.value_or(0));
		}
		const auto rec = *records + idx * 6;
		const auto before = Get<std::uint8_t>(rec + addr::kAmmoRecordLife).value_or(0);
		const auto beforeOn = (Get<std::uint8_t>(rec + addr::kAmmoRecordFlags).value_or(0) & addr::kAmmoRecordSuppressorOn) != 0;
		using SetUseFn = void (*)(std::uintptr_t, std::int32_t, std::int32_t, bool);
		using SetLifeFn = void (*)(std::uintptr_t, std::int32_t, std::int32_t, std::uint8_t);
		const auto setLife = [&] { reinterpret_cast<SetLifeFn>(addr::kAmmoSetSuppressorLife.va)(a_component, static_cast<std::int32_t>(a_player), a_slot, a_life); };
		const auto setUse = [&] { reinterpret_cast<SetUseFn>(addr::kAmmoSetUseSuppressor.va)(a_component, static_cast<std::int32_t>(a_player), a_slot, a_on); };
		if (beforeOn && !a_on) {
			// Turning it off: "off" first, then the life. SetSuppressorLife on a record that is still on calls its listener, and with a
			// life of 0 that is MGSV's "suppressor broke" notice (review 0.7.0: a false one at a boot that cleared a leftover).
			setUse();
			setLife();
		} else {
			// the life first, then "on": SetUseSuppressor's notice then shows the gauge at the new life
			setLife();
			if (beforeOn != a_on) {
				setUse();
			}
		}
		const auto after = Get<std::uint8_t>(rec + addr::kAmmoRecordLife).value_or(0);
		const auto afterOn = (Get<std::uint8_t>(rec + addr::kAmmoRecordFlags).value_or(0) & addr::kAmmoRecordSuppressorOn) != 0;
		const bool copy = vars::WriteSuppressorCopy(a_slot, after, afterOn);
		return std::format("slot {}: {}{} -> {}{}{}", a_slot, before, beforeOn ? " on" : " off", after, afterOn ? " on" : " off", copy ? "" : " (vars copy not written)");
	}

	namespace
	{
		struct Slot
		{
			bool          gun{ false };     // a gun slot whose record and gun data were read
			std::uint16_t equip{ 0 };
			std::uint8_t  life{ 0 };
			bool          on{ false };      // the record's "on" bit
			bool          takes{ false };   // the gun takes a suppressor (traits 0x40, durability > 0)
			std::uint8_t  full{ 0 };        // MGSV's full life for it (its gun data's durability)
			std::uint16_t bullet{ 0 };
		};

		// The slot's record and gun data (the listener's GunData replicated over safe reads; Addresses.h kAmmoListenerGunData).
		Slot ReadSlot(std::uintptr_t a_component, std::uint32_t a_player, int a_slot)
		{
			Slot       out;
			const auto per = Get<std::uint32_t>(a_component + 0xC);
			const auto types = Get<std::uintptr_t>(a_component + 0x40);
			const auto records = Get<std::uintptr_t>(a_component + 0x50);
			const auto listener = Get<std::uintptr_t>(a_component + 0x58);
			if (!per || *per < 3 || *per > 16 || !types || !*types || !records || !*records || !listener || !*listener) {
				return out;
			}
			const auto lvt = Get<std::uintptr_t>(*listener);
			if (!lvt || Get<std::uintptr_t>(*lvt + addr::kAmmoListenerVfGunData) != addr::kAmmoListenerGunData.va) {
				return out;
			}
			const std::uintptr_t idx = std::uintptr_t(*per) * a_player + std::uintptr_t(a_slot);
			const auto           type = Get<std::uint32_t>(*types + idx * 4);
			if (!type || (*type & 0x1F) < 1 || (*type & 0x1F) > 8) {
				return out;
			}
			const auto rec = *records + idx * 6;
			const auto life = Get<std::uint8_t>(rec + addr::kAmmoRecordLife);
			const auto flags = Get<std::uint8_t>(rec + addr::kAmmoRecordFlags);
			const auto inner = Get<std::uintptr_t>(*listener + 0x120);
			const auto table = inner && *inner ? Get<std::uintptr_t>(*inner + 0x28) : std::nullopt;
			const auto index = table && *table ? Get<std::uint8_t>(*table + std::uintptr_t(*type >> 16) * 0xE + 8) : std::nullopt;
			const auto base = Get<std::uintptr_t>(*listener + 0x1D8);
			if (!life || !flags || !index || !base || !*base) {
				return out;
			}
			const auto gun = *base + std::uintptr_t(*index) * addr::kGunDataBytes;
			const auto durability = Get<std::uint8_t>(gun + addr::kGunDataSuppressorLife);
			const auto traits = Get<std::uint8_t>(gun + addr::kGunDataTraits);
			const auto bullet = Get<std::uint16_t>(gun + addr::kGunDataBullet);
			if (!durability || !traits || !bullet) {
				return out;
			}
			out.gun = true;
			out.equip = static_cast<std::uint16_t>(*type >> 5 & 0x7FF);
			out.life = *life;
			out.on = (*flags & addr::kAmmoRecordSuppressorOn) != 0;
			out.takes = (*traits & addr::kWeaponTakesSuppressor) && *durability > 0;
			out.full = *durability;
			out.bullet = *bullet;
			return out;
		}

		// Every slot that has one (g_has) back to MGSV's full life; Lua thread.
		std::string RefillNow(const char* a_why)
		{
			std::string why;
			const auto  c = ammo::FindComponent(why);
			if (!c) {
				return std::format("{}: no ammo component ({})", a_why, why);
			}
			std::string out = a_why;
			for (int slot = 0; slot < 3; ++slot) {
				const auto equip = g_has[slot].load();
				if (!equip) {
					continue;
				}
				const auto s = ReadSlot(c->self, c->player, slot);
				if (!s.gun || s.equip != equip || !s.takes) {
					out += std::format("; slot {}: equip {} no longer there (now {})", slot, equip, s.gun ? s.equip : 0);
					g_has[slot].store(0);
					continue;
				}
				// A gun whose suppressor never wears (full 0xFF) counts as full only at 0xFF and on: a provisional or worn 254 is refilled
				// (review 0.7.0: it was skipped, and wore out for good after 254 shots).
				if (s.on && s.life >= s.full) {
					out += std::format("; slot {} (equip {}): full ({})", slot, equip, s.life);
					continue;
				}
				out += "; " + Set(c->self, c->player, slot, s.full, true) + std::format(" (equip {}, MGSV's full {})", equip, s.full);
			}
			if (out == a_why) {
				out += ": no suppressor this session";
			}
			return out;
		}
	}

	std::string ApplyLoadout(const std::string& a_mode, unsigned a_slots)
	{
		if (!g_setOk.load()) {
			return "setters unverified -- left as MGSV loaded it";
		}
		std::string why;
		const auto  c = ammo::FindComponent(why);
		if (!c) {
			return "no ammo component (" + why + ")";
		}
		std::string out;
		for (int slot = 0; slot < 3; ++slot) {
			if (!(a_slots & (1u << slot))) {
				continue;
			}
			const auto s = ReadSlot(c->self, c->player, slot);
			if (!s.gun) {
				out += std::format("{}slot {}: not readable", out.empty() ? "" : "; ", slot);
				continue;
			}
			// Every gun firing tranquilizer darts (logic::KindOfBullet: the handgun's 10 / 11 and the rifles', snipers', shotguns' ...;
			// review 0.7.0: only 10 / 11 counted); GuestScript's PR.SLEEP_BULLETS is the same list for PR.PreLoadout.
			const bool sleep = logic::KindOfBullet(s.bullet) == logic::BulletKind::kTranq;
			const bool want = s.takes && (a_mode == "all" || (a_mode == "sleep" && sleep));
			const auto prefix = std::format("{}slot {} equip {} (bullet {}, {}): ", out.empty() ? "" : "; ", slot, s.equip, s.bullet,
				s.takes ? std::format("takes one, MGSV's full {}", s.full) : std::string("takes none"));
			if (want) {
				g_has[slot].store(s.equip);
				// a provisional life from PR.PreLoadout (254), none, or off: MGSV's own full value; a life worn before a reload stays.
				// A gun whose suppressor never wears (full 0xFF): exactly 0xFF (MGSV skips the wear only there; a provisional 254 wore).
				if (!s.on || s.life == 0 || s.life > s.full || (s.full == addr::kSuppressorEndless && s.life != s.full)) {
					out += prefix + Set(c->self, c->player, slot, s.full, true);
				} else {
					out += prefix + std::format("on, life {} kept", s.life);
				}
			} else if (s.takes && s.full == addr::kSuppressorEndless) {
				// MGSV's own rule: such a gun always has its suppressor on (SetUseSuppressor forces it), and stripping it to life 0 only
				// made the reader call it worn out. Left on, as MGSV has it.
				g_has[slot].store(s.equip);
				out += prefix + (s.on && s.life == s.full ? std::string("never wears: left on") : Set(c->self, c->player, slot, s.full, true) + " (never wears)");
			} else {
				g_has[slot].store(0);
				if (s.on || s.life) {
					out += prefix + Set(c->self, c->player, slot, 0, false);
				} else {
					out += prefix + "none";
				}
			}
		}
		return out.empty() ? "no [Loadout] gun" : out;
	}

	void RequestRefill(const char* a_why)
	{
		if (std::string_view(a_why) == "rest" || std::string_view(a_why) == "respawn") {
			g_restAt.store(::GetTickCount64());
		}
		std::scoped_lock lock(g_lock);
		g_refillWhy = a_why;
	}

	void RefillOnLuaThread(const char* a_why)
	{
		if (!g_setOk.load()) {
			return;
		}
		if (std::string_view(a_why) == "rest" || std::string_view(a_why) == "respawn") {
			g_restAt.store(::GetTickCount64());
		}
		if (!SavesGuarded()) {
			Log::Info("suppressor: refill ({}): left as it is (MGSV's save guards are incomplete: it could be saved)", a_why);
			return;
		}
		auto out = RefillNow(a_why);
		// a pickup that changed nothing (the Infinity Bandana refills the reserve at every reload; the suppressor never wears then):
		// debug only
		if (std::string_view(a_why) != "pickup" || out.find(" -> ") != std::string::npos) {
			Log::Info("suppressor: refill ({})", out);
		} else {
			Log::Debug("suppressor: refill ({})", out);
		}
		std::scoped_lock lock(g_lock);
		++g_refills;
		g_lastRefill = std::move(out);
	}

	void NoteRest() { g_restAt.store(::GetTickCount64()); }

	void NoteStocks(const std::array<std::uint16_t, 23>& a_counts, bool a_valid)
	{
		// A supply drop's ammo (or any pickup) raises a weapon slot's stock (it counts every round, the magazine's too; a reload
		// or a shot never raises it): that gun's suppressor is refilled with it, as MGSV's own supply does. A rest's refill
		// raises it too (ignored for 3 s: the rest refills the suppressor itself).
		if (!a_valid) {
			g_lastStock = { -1, -1, -1 };
			return;
		}
		for (int slot = 0; slot < 3; ++slot) {
			const int now = a_counts[slot * 2];
			const int before = std::exchange(g_lastStock[slot], now);
			if (before >= 0 && now > before && g_has[slot].load() && ::GetTickCount64() - g_restAt.load() > 3000) {
				++g_pickups;
				Log::Debug("suppressor: slot {}'s stock rose {} -> {} (ammo picked up: a supply drop?) -- its suppressor is refilled too", slot, before, now);
				RequestRefill("pickup");
			}
		}
	}

	std::string Queue(int a_slot, int a_life, bool a_on)
	{
		if (a_slot < 0 || a_slot > 2 || a_life < 0 || a_life > 255) {
			return "usage: !suppressor <slot 0-2> <life 0-255> [on|off]";
		}
		std::scoped_lock lock(g_lock);
		g_qSlot = a_slot;
		g_qLife = a_life;
		g_qOn = a_on;
		return std::format("queued: slot {} life {} {} (next Lua update; !suppressor shows it)", a_slot, a_life, a_on ? "on" : "off");
	}

	void LuaTick()
	{
		int         slot, life;
		bool        on;
		std::string refill;
		{
			std::scoped_lock lock(g_lock);
			refill = std::exchange(g_refillWhy, {});
		}
		if (!refill.empty()) {
			RefillOnLuaThread(refill.c_str());
		}
		{
			std::scoped_lock lock(g_lock);
			if (g_qSlot < 0) {
				return;
			}
			slot = std::exchange(g_qSlot, -1);
			life = g_qLife;
			on = g_qOn;
		}
		std::string why;
		const auto  c = ammo::FindComponent(why);
		auto        out = !SavesGuarded() ? std::string("left as it is (MGSV's save guards are incomplete: it could be saved)") :
		                  c                ? Set(c->self, c->player, slot, static_cast<std::uint8_t>(life), on) :
		                                     "no ammo component (" + why + ")";
		Log::Info("suppressor: console set: {}", out);
		std::scoped_lock lock(g_lock);
		g_lastSet = std::move(out);
	}

	std::string LastSet()
	{
		std::scoped_lock lock(g_lock);
		return g_lastSet;
	}

	void NoteInHand(const Reading& a_r)
	{
		const std::uint64_t key = a_r.valid ? (std::uint64_t(a_r.equip) | std::uint64_t(a_r.slot) << 16 | std::uint64_t(a_r.on) << 24 |
		                                          std::uint64_t(a_r.life == 0 ? 0 : 1) << 32)
		                                    : 0xFFFFFFFFull << 33;
		if (key == g_lastInHand) {
			return;
		}
		g_lastInHand = key;
		if (a_r.Working() && a_r.slot < 3 && !g_has[a_r.slot].load()) {
			g_has[a_r.slot].store(a_r.equip);  // a suppressor the player has (MGSV's, a supply drop's): refilled with the rest
		}
		if (a_r.valid && a_r.firearm) {
			Log::Info("suppressor: in hand equip {} (slot {}): {}", a_r.equip, a_r.slot,
				a_r.Working() ? std::format("on, life {}", a_r.life) : a_r.on ? "on but worn out" : std::format("none or off (life {})", a_r.life));
		}
	}

	std::string Describe(std::uintptr_t a_character, std::uint32_t a_player)
	{
		// The console's worker thread: the Lua thread's refill writes these under g_lock (review 0.7.0: read without it).
		std::string   lastRefill;
		std::uint64_t refills = 0;
		{
			std::scoped_lock lock(g_lock);
			lastRefill = g_lastRefill;
			refills = g_refills;
		}
		std::string out = std::format("suppressor: reader {}, setters {}; has (slot: equip) 0:{} 1:{} 2:{}; {} refills, {} pickups, last {}",
			g_ok.load() ? "verified" : "OFF", g_setOk.load() ? "verified" : "OFF", g_has[0].load(), g_has[1].load(), g_has[2].load(), refills, g_pickups.load(),
			lastRefill);
		std::string why;
		if (const auto c = ammo::FindComponent(why)) {
			const auto per = Get<std::uint32_t>(c->self + 0xC).value_or(0);
			const auto types = Get<std::uintptr_t>(c->self + 0x40).value_or(0);
			const auto records = Get<std::uintptr_t>(c->self + 0x50).value_or(0);
			const auto listener = Get<std::uintptr_t>(c->self + 0x58).value_or(0);
			const auto lvt = listener ? Get<std::uintptr_t>(listener).value_or(0) : 0;
			out += std::format("; component {:#x} player {} slots {} listener {:#x} (vtable {:#x}, vf 0x150 {:#x})", c->self, c->player, per, listener, lvt,
				lvt ? Get<std::uintptr_t>(lvt + 0x150).value_or(0) : 0);
			for (int s = 0; s < 3 && per >= 3 && types && records; ++s) {
				const std::uintptr_t idx = std::uintptr_t(per) * c->player + s;
				const auto           rec = records + idx * 6;
				const auto           sl = ReadSlot(c->self, c->player, s);
				out += std::format("; slot {} type {:#x} mag {} sub {} life {} flags {:#04x}{}", s, Get<std::uint32_t>(types + idx * 4).value_or(0),
					Get<std::uint16_t>(rec).value_or(0), Get<std::uint16_t>(rec + 2).value_or(0), Get<std::uint8_t>(rec + 4).value_or(0),
					Get<std::uint8_t>(rec + 5).value_or(0),
					sl.gun ? std::format(" (equip {} bullet {} {})", sl.equip, sl.bullet, sl.takes ? std::format("takes one, full {}", sl.full) : std::string("takes none")) : std::string());
			}
		} else {
			out += "; no ammo component (" + why + ")";
		}
		const auto v = vars::Read();
		if (v.suppressorOk) {
			out += std::format("; vars lives {} {} {} on {} {} {}", v.suppressorLives[0], v.suppressorLives[1], v.suppressorLives[2], v.isUseSuppressor[0],
				v.isUseSuppressor[1], v.isUseSuppressor[2]);
		}
		const auto r = InHand(a_character, a_player);
		if (r.valid) {
			out += std::format("; in hand equip {} slot {} {}", r.equip, r.slot,
				!r.firearm ? std::string("(not a firearm)") : r.Working() ? std::format("ON life {}", r.life) : std::format("off (on {} life {})", r.on, r.life));
		} else {
			out += "; in hand: unreadable";
		}
		return out;
	}
}
